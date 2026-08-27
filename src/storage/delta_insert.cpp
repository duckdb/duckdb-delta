#include "storage/delta_insert.hpp"

#include "duckdb/common/sorting/hashed_sort.hpp"
#include "duckdb/common/path.hpp"

#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "functions/delta_scan/delta_scan.hpp"
#include "duckdb/execution/physical_operator_states.hpp"

#include "storage/delta_catalog.hpp"
#include "storage/delta_transaction.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "storage/delta_table_entry.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

namespace duckdb {

DeltaInsert::DeltaInsert(PhysicalPlan &plan, LogicalOperator &op, TableCatalogEntry &table,
                         physical_index_vector_t<idx_t> column_index_map_p)
    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(&table), schema(nullptr),
      column_index_map(std::move(column_index_map_p)) {
}

DeltaInsert::DeltaInsert(PhysicalPlan &plan, LogicalOperator &op, SchemaCatalogEntry &schema,
                         unique_ptr<BoundCreateTableInfo> info)
    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(nullptr), schema(&schema),
      info(std::move(info)) {
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class DeltaInsertGlobalState : public GlobalSinkState {
public:
	explicit DeltaInsertGlobalState(const DeltaTableEntry &table)
	    : table_name(table.name), not_null_constraints(table.GetNotNullConstraints()) {
		table.ThrowOnUnsupportedFieldForInserting();

		columns = table.snapshot->GetLazyLoadedGlobalColumns();
	};

	string table_name;

	vector<DeltaDataFile> written_files;

	vector<DeltaMultiFileColumnDefinition> columns;

	idx_t insert_count = 0;

	// Fields in the table with not null constraints. These
	case_insensitive_map_t<vector<NestedNotNullConstraint>> not_null_constraints;
};

unique_ptr<GlobalSinkState> DeltaInsert::GetGlobalSinkState(ClientContext &context) const {
	// TODO: handle null table once CREATE TABLE (AS SELECT) is supported; columns/constraints come from info->Base()
	const auto &delta_table = table->Cast<DeltaTableEntry>();
	return make_uniq<DeltaInsertGlobalState>(delta_table);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
static string ParseQuotedValue(const string &input, idx_t &pos) {
	if (pos >= input.size() || input[pos] != '"') {
		throw InvalidInputException("Failed to parse quoted value - expected a quote");
	}
	string result;
	pos++;
	for (; pos < input.size(); pos++) {
		if (input[pos] == '"') {
			pos++;
			// check if this is an escaped quote
			if (pos < input.size() && input[pos] == '"') {
				// escaped quote
				result += '"';
				continue;
			}
			return result;
		}
		result += input[pos];
	}
	throw InvalidInputException("Failed to parse quoted value - unterminated quote");
}

static vector<string> ParseQuotedList(const string &input, char list_separator) {
	vector<string> result;
	if (input.empty()) {
		return result;
	}
	idx_t pos = 0;
	while (true) {
		result.push_back(ParseQuotedValue(input, pos));
		if (pos >= input.size()) {
			break;
		}
		if (input[pos] != list_separator) {
			throw InvalidInputException("Failed to parse list - expected a %s", string(1, list_separator));
		}
		pos++;
	}
	return result;
}

static DeltaColumnStats ParseColumnStats(const vector<Value> col_stats) {
	DeltaColumnStats column_stats;
	for (idx_t stats_idx = 0; stats_idx < col_stats.size(); stats_idx++) {
		auto &stats_children = StructValue::GetChildren(col_stats[stats_idx]);
		auto &stats_name = StringValue::Get(stats_children[0]);
		auto &stats_value = StringValue::Get(stats_children[1]);
		if (stats_name == "min") {
			D_ASSERT(!column_stats.has_min);
			column_stats.min = stats_value;
			column_stats.has_min = true;
		} else if (stats_name == "max") {
			D_ASSERT(!column_stats.has_max);
			column_stats.max = stats_value;
			column_stats.has_max = true;
		} else if (stats_name == "null_count") {
			D_ASSERT(!column_stats.has_null_count);
			column_stats.has_null_count = true;
			column_stats.null_count = StringUtil::ToUnsigned(stats_value);
		} else if (stats_name == "column_size_bytes") {
			column_stats.column_size_bytes = StringUtil::ToUnsigned(stats_value);
		} else if (stats_name == "has_nan") {
			column_stats.has_contains_nan = true;
			column_stats.contains_nan = stats_value == "true";
		} else if (stats_name == "num_values") {
			D_ASSERT(!column_stats.has_num_values);
			column_stats.has_num_values = true;
			column_stats.num_values = StringUtil::ToUnsigned(stats_value);
		} else if (stats_name == "min_is_exact" || stats_name == "max_is_exact") {
			// Parquet reports these false only for strings cut off at the writer's stats size limit, which
			// PROTOCOL.md (Per-file Statistics, note 1) keeps inside tightBounds=true -- bounds stay tight.
			// Thus accept and drop.
			continue;
		} else if (stats_name == "variant_type") {
			//! Should be handled elsewhere
			continue;
		} else {
			throw NotImplementedException("Unsupported stats type \"%s\" in DeltaInsert::Sink()", stats_name);
		}
	}
	return column_stats;
}

static void AddWrittenFiles(DeltaInsertGlobalState &global_state, DataChunk &chunk) {
	for (idx_t r = 0; r < chunk.size(); r++) {
		DeltaDataFile data_file;
		data_file.file_name = chunk.GetValue(0, r).GetValue<string>();
		data_file.row_count = chunk.GetValue(1, r).GetValue<idx_t>();
		data_file.file_size_bytes = chunk.GetValue(2, r).GetValue<idx_t>();
		data_file.footer_size = chunk.GetValue(3, r).GetValue<idx_t>();
		// extract the column stats
		auto column_stats = chunk.GetValue(4, r);
		auto &map_children = MapValue::GetChildren(column_stats);

		global_state.insert_count += data_file.row_count;

		for (idx_t col_idx = 0; col_idx < map_children.size(); col_idx++) {
			auto &struct_children = StructValue::GetChildren(map_children[col_idx]);
			auto &col_name = StringValue::Get(struct_children[0]);
			auto &col_stats = MapValue::GetChildren(struct_children[1]);
			auto column_names = ParseQuotedList(col_name, '.');
			auto stats = ParseColumnStats(col_stats);

			// The copy reports stats under the names it was given, which on a column-mapped table are the
			// physical ones -- and that is also how the log wants them keyed, so they pass through untouched.
			// Constraints and messages resolve on the logical name, which would otherwise miss silently and
			// stop enforcing the constraint.
			bool found = false;
			LogicalType coltype;
			string logical_name;
			for (auto &col : global_state.columns) {
				const auto &written_name = col.physical_name.empty() ? col.name.GetIdentifierName() : col.physical_name;
				if (written_name == column_names[0]) {
					found = true;
					coltype = col.type;
					logical_name = col.name.GetIdentifierName();
					break;
				}
			}
			if (!found) {
				throw InternalException("Column %s not found in table %s", StringUtil::Join(column_names, "."),
				                        global_state.table_name);
			}

			if (stats.has_null_count && stats.null_count > 0) {
				auto constraint = global_state.not_null_constraints.find(logical_name);
				if (constraint != global_state.not_null_constraints.end()) {
					// We may have a not null constraint for this col, it's not nested so it
					if (column_names.size() == 1) {
						throw ConstraintException("NOT NULL constraint failed: %s.%s", global_state.table_name,
						                          logical_name);
					}

					// Check paths
					for (auto &constr : constraint->second) {
						if (col_name == constr.path) {
							auto logical_path = column_names;
							logical_path[0] = logical_name;
							throw ConstraintException("NOT NULL constraint failed: %s.%s", global_state.table_name,
							                          StringUtil::Join(logical_path, "."));
						}
					}
				}
			}

			// Skip types whose stats we don't yet support
			if (coltype.id() == LogicalTypeId::VARIANT || coltype.id() == LogicalTypeId::LIST) {
				continue;
			}

			stats.root_type = coltype;

			// Push the columns stats into the datafile
			data_file.column_stats.push_back({std::move(column_names), std::move(stats)});
		}

		// extract the partition info
		auto partition_info = chunk.GetValue(5, r);
		if (!partition_info.IsNull()) {
			auto &partition_children = MapValue::GetChildren(partition_info);
			for (idx_t col_idx = 0; col_idx < partition_children.size(); col_idx++) {
				auto &struct_children = StructValue::GetChildren(partition_children[col_idx]);

				DeltaPartition file_partition_info;
				file_partition_info.partition_column_idx = col_idx;
				// The kernel reads a null map value as a null partition and "" as the empty string. The
				// protocol's "an empty string for any type translates to a null partition value" cannot
				// hold for STRING columns, where "" is itself a legal value.
				if (!struct_children[1].IsNull()) {
					file_partition_info.partition_value = StringValue::Get(struct_children[1]);
				}
				data_file.partition_values.push_back(std::move(file_partition_info));
			}
		}

		global_state.written_files.push_back(std::move(data_file));
	}
}

SinkResultType DeltaInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &global_state = input.global_state.Cast<DeltaInsertGlobalState>();

	if (chunk.size() != 1) {
		throw InternalException(
		    "DeltaInsert::Sink expects a single row containing output of the PhysicalCopy that should be its Source");
	}

	AddWrittenFiles(global_state, chunk);

	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// GetDataInternal
//===--------------------------------------------------------------------===//
SourceResultType DeltaInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                              OperatorSourceInput &input) const {
	auto &global_state = sink_state->Cast<DeltaInsertGlobalState>();
	auto value = Value::BIGINT(global_state.insert_count);
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, value);
	return SourceResultType::FINISHED;
}
//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType DeltaInsert::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                       OperatorSinkFinalizeInput &input) const {
	auto &global_state = input.global_state.Cast<DeltaInsertGlobalState>();

	// TODO: handle null table once CREATE TABLE (AS SELECT) is supported; create the table first, then use
	// schema->catalog
	auto &transaction = DeltaTransaction::Get(context, table->catalog);
	vector<string> filenames;
	transaction.Append(context, global_state.written_files);

	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string DeltaInsert::GetName() const {
	return table ? "DELTA_INSERT" : "DELTA_CREATE_TABLE_AS";
}

InsertionOrderPreservingMap<string> DeltaInsert::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table ? table->name.GetIdentifierName() : info->Base().GetTableName().GetIdentifierName();
	return result;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//
static optional_ptr<CopyFunctionCatalogEntry> TryGetCopyFunction(DatabaseInstance &db, const string &name) {
	D_ASSERT(!name.empty());
	auto &system_catalog = Catalog::GetSystemCatalog(db);
	auto data = CatalogTransaction::GetSystemTransaction(db);
	auto &schema = system_catalog.GetSchema(data, Identifier::DefaultSchema());
	return schema.GetEntry(data, CatalogType::COPY_FUNCTION_ENTRY, Identifier(name))->Cast<CopyFunctionCatalogEntry>();
}

namespace {

struct DeltaStringWidthCheckData : public FunctionData {
	DeltaStringWidthCheckData(string column_name_p, string declared_type_p, idx_t max_length_p)
	    : column_name(std::move(column_name_p)), declared_type(std::move(declared_type_p)), max_length(max_length_p) {
	}

	string column_name;
	string declared_type;
	idx_t max_length;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DeltaStringWidthCheckData>(column_name, declared_type, max_length);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<DeltaStringWidthCheckData>();
		return column_name == other.column_name && declared_type == other.declared_type &&
		       max_length == other.max_length;
	}
};

//! Reject rather than truncate: Spark re-checks the width on every rewrite, so a truncating write would trade silent
//! data loss for a table the reference writer later refuses.
void DeltaStringWidthCheck(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<DeltaStringWidthCheckData>();

	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(input);
	auto strings = UnifiedVectorFormat::GetData<string_t>(input);
	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = input.sel->get_index(i);
		if (!input.validity.RowIsValid(idx)) {
			continue;
		}
		// Codepoints, matching both Spark's char/varchar length and DuckDB's length()
		auto length = Length<string_t, idx_t>(strings[idx]);
		if (length > info.max_length) {
			throw InvalidInputException("Delta column \"%s\" is declared as %s, but the value being written is %llu "
			                            "characters long. Delta records this width as __CHAR_VARCHAR_TYPE_STRING "
			                            "field metadata; writing a longer value produces a table that Spark rejects.",
			                            info.column_name, info.declared_type, length);
		}
	}
	result.Reference(args.data[0]);
}

//! Deliberately not registered in the catalog: it exists only inside a physical plan built here, so there is no name
//! to resolve and no user-facing function to misuse. Registering it would add surface, not safety.
ScalarFunction GetStringWidthCheckFunction() {
	ScalarFunction function("delta_check_string_width", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                        DeltaStringWidthCheck);
	function.SetFallible();
	return function;
}

} // namespace

//! Adapts the insert child plan to what the parquet copy expects. Any further per-column rewrite on the write path
//! belongs here, between the child and the copy: the sink only ever sees the copy's written-file summary, never data.
static PhysicalOperator &PlanInsertProjection(PhysicalPlanGenerator &planner, PhysicalOperator &child,
                                              const ColumnList &columns,
                                              const vector<DeltaStringWidthBound> &width_bounds) {
	if (width_bounds.empty()) {
		return child;
	}

	auto types = child.GetTypes();
	if (types.size() != columns.PhysicalColumnCount()) {
		// The binder resolves the child into table order and width, so this should not fire; refuse the write rather
		// than check widths against columns we cannot line up.
		throw BinderException("Cannot write to Delta table with declared CHAR/VARCHAR widths: the insert produces %llu "
		                      "columns but the table has %llu",
		                      types.size(), columns.PhysicalColumnCount());
	}

	vector<unique_ptr<Expression>> expressions;
	for (idx_t i = 0; i < types.size(); i++) {
		expressions.push_back(make_uniq<BoundReferenceExpression>(types[i], i));
	}

	for (const auto &width_bound : width_bounds) {
		auto &column = columns.GetColumn(PhysicalIndex(width_bound.column_index));
		if (!width_bound.max_length.IsValid()) {
			auto declared =
			    width_bound.declared_type.empty()
			        ? "has a nested field declaring a CHAR/VARCHAR width"
			        : StringUtil::Format("declares the width %s on a nested field", width_bound.declared_type);
			throw NotImplementedException("Delta column \"%s\" %s, which duckdb-delta cannot enforce on write yet. "
			                              "Refusing the write rather than committing values Spark would reject.",
			                              column.Name().GetIdentifierName(), declared);
		}

		vector<unique_ptr<Expression>> check_children;
		check_children.push_back(std::move(expressions[width_bound.column_index]));
		expressions[width_bound.column_index] = make_uniq<BoundFunctionExpression>(
		    BoundScalarFunction(GetStringWidthCheckFunction()), std::move(check_children),
		    make_uniq<DeltaStringWidthCheckData>(column.Name().GetIdentifierName(), width_bound.declared_type,
		                                         width_bound.max_length.GetIndex()));
	}

	auto &projection =
	    planner.Make<PhysicalProjection>(std::move(types), std::move(expressions), child.estimated_cardinality);
	projection.children.push_back(child);
	return projection;
}

//! Rewrites the parquet column names to their physical names and attaches the field ids, for a table that uses
//! column mapping. Nested and partitioned mapped tables reach here too: they are only mapped in part, and
//! DeltaTableEntry::ThrowOnUnsupportedFieldForInserting refuses them later, when the insert executes.
static void ApplyColumnMappingToWriteSchema(DeltaTableEntry &table_entry, CopyInfo &info, vector<string> &names) {
	// The snapshot schema, which the sink resolves its stats against too -- one source of truth for physical
	// names at both ends of the write. The kernel's write context is the more direct answer but is not usable
	// here: the only accessor is the unpartitioned one, which throws outright on a partitioned table.
	auto &schema = table_entry.snapshot->GetLazyLoadedGlobalColumns();

	unordered_map<string, const_reference<DeltaMultiFileColumnDefinition>> by_logical_name;
	bool mapped = false;
	for (auto &col : schema) {
		by_logical_name.emplace(col.name.GetIdentifierName(), col);
		mapped |= col.IsColumnMapped();
	}
	if (!mapped) {
		return;
	}

	child_list_t<Value> field_ids;
	for (auto &name : names) {
		// Paired by name rather than by position: the two schemas are visited separately -- this one from the
		// scan, `names` from the catalog entry's bind -- and pairing them positionally would silently write a
		// column under its neighbour's identity if they ever diverged.
		auto entry = by_logical_name.find(name);
		if (entry == by_logical_name.end()) {
			throw InternalException("Delta column \"%s\" is missing from the write schema of table %s", name,
			                        table_entry.name.GetIdentifierName());
		}
		auto &col = entry->second.get();
		if (!col.physical_name.empty()) {
			name = col.physical_name;
		}
		if (col.field_id.IsValid()) {
			// Keyed by whatever this column is actually written as, so the two cannot drift. Checked cast: the
			// protocol's ids are 32-bit, so a wider one means a malformed log, not a value to silently truncate
			// into a different column's id.
			field_ids.emplace_back(name, Value::INTEGER(NumericCast<int32_t>(col.field_id.GetIndex())));
		}
	}

	if (!field_ids.empty()) {
		info.options["field_ids"] = {Value::STRUCT(std::move(field_ids))};
	}
}

PhysicalOperator &DeltaCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                           optional_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for insertion into Delta table");
	}
	if (op.on_conflict_info.action_type != OnConflictAction::THROW) {
		throw BinderException("ON CONFLICT clause not yet supported for insertion into Delta table");
	}

	// Lookup table here:
	optional_ptr<DeltaTableEntry> table_entry;
	if (child_catalog_mode) {
		// In child catalog mode, the LogicalInsert will not actually contain the table entry, so we need to look it up
		// here
		CatalogEntryRetriever retriever(context);
		EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, Identifier(default_table));
		auto default_table_entry =
		    LookupEntry(retriever, default_schema, lookup_info, OnEntryNotFound::THROW_EXCEPTION);
		table_entry = default_table_entry.entry->Cast<DeltaTableEntry>();
	} else {
		table_entry = op.table.Cast<DeltaTableEntry>();
	}

	string delta_path = Path::Normalize(table_entry->snapshot->GetPath());

	// Create Copy Info
	auto info = make_uniq<CopyInfo>();
	info->file_path = delta_path;
	info->format = "parquet";
	info->is_from = false;

	// Get Parquet Copy function
	auto copy_fun = TryGetCopyFunction(*context.db, "parquet");
	if (!copy_fun) {
		throw MissingExtensionException("Did not find parquet copy function required to write to delta table");
	}

	auto partitions = table_entry->snapshot->GetPartitionColumns();
	vector<idx_t> partition_columns;
	if (!partitions.empty()) {
		auto column_names = table_entry->GetColumns().GetColumnNames();
		for (int64_t i = 0; i < partitions.size(); i++) {
			for (int64_t j = 0; j < column_names.size(); j++) {
				if (column_names[j] == partitions[i]) {
					partition_columns.push_back(j);
					break;
				}
			}
		}
	}

	// Bind Copy Function
	auto &columns = table_entry->GetColumns();

	auto names_to_write = columns.GetColumnNames();
	auto types_to_write = columns.GetColumnTypes();

	ApplyColumnMappingToWriteSchema(*table_entry, *info, names_to_write);

	CopyFunctionBindInput bind_input(*info);

	auto function_data =
	    copy_fun->function.copy_to_bind(context, bind_input, StringsToIdentifiers(names_to_write), types_to_write);

	auto &insert = planner.Make<DeltaInsert>(op, *table_entry, op.column_index_map);

	// Note: this is quite hacky, in the current setup we are expecting the op.table entry to be the entry in the parent
	//       catalog. We pass through the pointer to this table entry because we need this on commit.
	if (parent_commit) {
		auto &delta_transaction = Transaction::Get(context, table_entry->catalog).Cast<DeltaTransaction>();
		delta_transaction.SetParentTableEntry(op.table);
	}

	auto &physical_copy = planner.Make<PhysicalCopyToFile>(
	    GetCopyFunctionReturnLogicalTypes(CopyFunctionReturnType::WRITTEN_FILE_STATISTICS), copy_fun->function,
	    std::move(function_data), op.estimated_cardinality);
	auto &physical_copy_ref = physical_copy.Cast<PhysicalCopyToFile>();

	auto current_write_uuid = UUID::ToString(UUID::GenerateRandomUUID());

	physical_copy_ref.use_tmp_file = false;
	if (!partition_columns.empty()) {
		physical_copy_ref.filename_pattern.SetFilenamePattern("duckdb_" + current_write_uuid + "_{i}");
		physical_copy_ref.file_path = delta_path;
		physical_copy_ref.partition_output = true;
		physical_copy_ref.partition_columns = partition_columns;
		physical_copy_ref.write_empty_file = true;
	} else {
		physical_copy_ref.file_path =
		    Path::FromString(delta_path).Join("duckdb-" + current_write_uuid + ".parquet").ToString();
		physical_copy_ref.partition_output = false;
		physical_copy_ref.write_empty_file = false;
	}

	physical_copy_ref.file_extension = "parquet";
	physical_copy_ref.overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE;
	physical_copy_ref.per_thread_output = false;
	physical_copy_ref.return_type = CopyFunctionReturnType::WRITTEN_FILE_STATISTICS;
	physical_copy_ref.write_partition_columns = true;
	physical_copy_ref.children.push_back(
	    PlanInsertProjection(planner, *plan, columns, table_entry->snapshot->GetStringWidthBounds()));
	physical_copy_ref.names = StringsToIdentifiers(names_to_write);
	physical_copy_ref.expected_types = types_to_write;
	physical_copy_ref.hive_file_pattern = true;

	insert.children.push_back(physical_copy);

	return insert;
}

} // namespace duckdb
