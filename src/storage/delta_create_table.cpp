#include "storage/delta_create_table.hpp"

#include "delta_schema_builder.hpp"
#include "delta_table_properties.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "storage/delta_catalog.hpp"
#include "storage/delta_insert.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/table_function_binder.hpp"

namespace duckdb {

//! Canonical form for comparing two spellings of one table location. `Path::ToString` is base +
//! path + trailing separator, so dropping the separator is just leaving the last part off.
static string CanonicalTablePath(const string &path) {
	auto parsed = Path::FromString(path);
	return parsed.GetBase() + parsed.GetPath();
}

//! Table options reach the catalog as unevaluated parsed expressions, so bind and evaluate each one
//! here. An option then accepts anything that evaluates to a constant, not only a literal.
static Value ParseCreateTableOption(ClientContext &context, TableFunctionBinder &binder,
                                    const ParsedExpression &expression, const string &name, const LogicalType &type) {
	auto copy = expression.Copy();
	auto bound = binder.Bind(copy);
	auto value = ExpressionExecutor::EvaluateScalar(context, *bound, true);
	if (value.IsNull()) {
		throw BinderException("NULL is not a valid value for Delta CREATE TABLE option '%s'", name);
	}
	return value.DefaultCastAs(type);
}

//! Resolves the destination path for a CREATE TABLE. Defaults to the attached path; `WITH (location =
//! '...')` names it explicitly, spelled as the Iceberg catalog spells it. `path` named the same thing
//! before this, and it never shipped, so it is refused rather than taken for a table property.
static string GetCreateTablePath(ClientContext &context, TableFunctionBinder &binder, const CreateTableInfo &base,
                                 DeltaCatalog &delta_catalog) {
	if (base.options.find("path") != base.options.end()) {
		throw BinderException("Delta CREATE TABLE names the destination with 'location', not 'path'");
	}
	auto option = base.options.find("location");
	if (option == base.options.end()) {
		return delta_catalog.GetDBPath();
	}
	auto path = ParseCreateTableOption(context, binder, *option->second, option->first, LogicalType::VARCHAR)
	                .GetValue<string>();

	// A Delta catalog is a single table at a single path, so a divergent path would produce a
	// catalog entry that does not describe what was attached. Compare normalized, so a trailing
	// slash or a relative spelling of the attached path is not a spurious mismatch.
	auto attached_path = delta_catalog.GetDBPath();
	if (CanonicalTablePath(path) != CanonicalTablePath(attached_path)) {
		throw NotImplementedException(
		    "Delta CREATE TABLE can only create a table at the attached path ('%s'), not at '%s'", attached_path, path);
	}
	return path;
}

//! Every option that does not name the location is a Delta table property, handed to kernel untouched:
//! kernel decides which keys it recognizes and derives the protocol from them. Sorted so that two bad
//! properties always fail in the same order; the written configuration is a map kernel orders itself.
static vector<pair<string, string>> GetCreateTableProperties(ClientContext &context, TableFunctionBinder &binder,
                                                             const CreateTableInfo &base) {
	vector<pair<string, string>> properties;
	for (auto &option : base.options) {
		if (StringUtil::CIEquals(option.first, "location")) {
			continue;
		}
		// A `delta.*` key must be one Delta has, spelled exactly; any other key is the user's own property and is kept
		// as given, as COPY TO's WITH_PROPERTIES keeps it
		DeltaTableProperties::ThrowIfUnknownDeltaKey(option.first, "Delta CREATE TABLE");
		auto value = ParseCreateTableOption(context, binder, *option.second, option.first, LogicalType::VARCHAR);
		properties.emplace_back(option.first, value.GetValue<string>());
	}
	std::sort(properties.begin(), properties.end());
	return properties;
}

static vector<string> GetCreateTablePartitionColumns(const CreateTableInfo &base) {
	vector<string> result;
	for (auto &key : base.partition_keys) {
		if (key->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			throw BinderException("Delta CREATE TABLE only supports plain column names in PARTITIONED BY, found '%s'",
			                      key->ToString());
		}
		auto &colref = key->Cast<ColumnRefExpression>();
		if (colref.IsQualified()) {
			throw BinderException("Delta CREATE TABLE does not support qualified partition columns ('%s')",
			                      key->ToString());
		}
		result.push_back(colref.GetColumnName().GetIdentifierName());
	}
	return result;
}

DeltaCreateTableRequest BindDeltaCreateTable(ClientContext &context, DeltaCatalog &delta_catalog,
                                             const CreateTableInfo &base) {
	if (delta_catalog.access_mode == AccessMode::READ_ONLY) {
		throw InvalidInputException("Can not create a table in a read only Delta catalog");
	}

	// LookupEntry only ever resolves the single attached table, so any other name would create a
	// table that could never be read back.
	auto table_name = base.GetTableName().GetIdentifierName();
	if (table_name != delta_catalog.GetName() && table_name != delta_catalog.internal_table_name) {
		throw NotImplementedException("Delta CREATE TABLE must use the attached name ('%s'), found '%s'",
		                              delta_catalog.GetName().GetIdentifierName(), table_name);
	}
	// Delta stores a default or a generation expression only with the `allowColumnDefaults` or
	// `generatedColumns` writer feature, which we do not request: without one the table would be
	// created without what the statement asked for, and an omitted value would read as NULL.
	for (auto &column : base.columns.Logical()) {
		if (column.Generated()) {
			throw NotImplementedException("Delta CREATE TABLE does not support generated columns ('%s')",
			                              column.GetName().GetIdentifierName());
		}
		if (column.HasDefaultValue()) {
			throw NotImplementedException("Delta CREATE TABLE does not support DEFAULT values ('%s')",
			                              column.GetName().GetIdentifierName());
		}
	}
	// DeltaCatalog::SupportsCreateTable refuses SORTED BY too, but the binder consults it for CREATE only, not for
	// CREATE TABLE AS. Delta has partition columns, but nothing that SORTED BY maps onto.
	if (!base.sort_keys.empty()) {
		throw CatalogException("SORTED BY is not supported for tables in a delta catalog");
	}
	for (auto &constraint : base.constraints) {
		// Delta requires columns to be nullable unless the `invariants` writer feature is enabled,
		// which we do not request yet.
		if (constraint->type == ConstraintType::NOT_NULL) {
			throw NotImplementedException("Delta CREATE TABLE does not support NOT NULL constraints");
		}
		throw NotImplementedException("Delta CREATE TABLE does not support constraints");
	}

	auto binder = Binder::CreateBinder(context);
	TableFunctionBinder option_binder(*binder, context, "CREATE TABLE", "Table option");
	DeltaCreateTableRequest request;
	request.path = GetCreateTablePath(context, option_binder, base, delta_catalog);
	request.partition_columns = GetCreateTablePartitionColumns(base);
	request.properties = GetCreateTableProperties(context, option_binder, base);
	request.columns = base.columns.Copy();

	if (base.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		// Replacing means dropping, and Delta tables do not support dropping yet.
		throw NotImplementedException("Delta tables do not support CREATE OR REPLACE TABLE");
	}
	return request;
}

//! The first type below `type` that the Delta schema has no type for, or nullptr. Mirrors DeltaSchemaBuilder.
static optional_ptr<const LogicalType> FindUnstorableType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::VARIANT:
		return nullptr;
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			auto result = FindUnstorableType(child.second);
			if (result) {
				return result;
			}
		}
		return nullptr;
	case LogicalTypeId::LIST:
		return FindUnstorableType(ListType::GetChildType(type));
	case LogicalTypeId::MAP: {
		auto result = FindUnstorableType(MapType::KeyType(type));
		return result ? result : FindUnstorableType(MapType::ValueType(type));
	}
	default:
		return &type;
	}
}

void ThrowIfDeltaCannotStore(const ColumnList &columns, const string &statement, const string &hint) {
	for (auto &column : columns.Logical()) {
		auto &type = column.Type();
		auto unstorable = FindUnstorableType(type);
		if (!unstorable) {
			continue;
		}
		auto nested =
		    unstorable.get() == &type ? string() : StringUtil::Format(" (it holds a %s)", unstorable->ToString());
		throw NotImplementedException("%s does not support column '%s' of type %s%s: Delta has no such type. %s",
		                              statement, column.Name().GetIdentifierName(), type.ToString(), nested, hint);
	}
}

bool DeltaTableExistsAt(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto log_path = Path::FromString(path).Join("_delta_log");
	if (Path::FromString(path).IsLocal()) {
		return fs.DirectoryExists(log_path.ToString());
	}
	// An object store has no directories, and S3's DirectoryExists answers true for every prefix, so
	// judge by content instead, as kernel does: any file under the log, a staged commit in `_commits`
	// included, means the table is there.
	return !fs.Glob(log_path.Join("**").ToString()).empty();
}

bool EnsureTableDirectory(ClientContext &context, const string &path) {
	if (!Path::FromString(path).IsLocal()) {
		return false;
	}
	auto &fs = FileSystem::GetFileSystem(context);
	if (fs.DirectoryExists(path)) {
		return false;
	}
	fs.CreateDirectoriesRecursive(path);
	return true;
}

DeltaCreateTableTransaction::DeltaCreateTableTransaction(ClientContext &context, const DeltaCreateTableRequest &request,
                                                         const CommitterFactory &committer)
    : path(request.path), data_change(request.data_change), partition_columns(request.partition_columns),
      columns(request.columns.Copy()) {
	for (auto &partition_column : partition_columns) {
		for (auto &column : columns.Logical()) {
			if (column.Name().GetIdentifierName() == partition_column) {
				partition_types.push_back(column.Type());
				break;
			}
		}
	}

	// Assigned, not constructed: the pointer's move constructor drops its deleter
	engine = CreateDeltaEngine(context, path);

	// The builder holds a borrowed pointer to `schema_builder`, and kernel runs the visitor during
	// new_create_table_txn_builder -- both must outlive that call.
	DeltaSchemaBuilder schema_builder(columns);
	auto engine_schema = schema_builder.CreateEngineSchema();

	ffi::ExclusiveCreateTableTransactionBuilder *create_builder;
	auto builder_res = KernelUtils::TryUnpackResult(
	    ffi::new_create_table_txn_builder(KernelUtils::ToDeltaString(path), &engine_schema,
	                                      KernelUtils::ToDeltaString("DuckDB"), request.data_change, engine.get()),
	    create_builder);
	if (builder_res.HasError()) {
		// A schema-lowering failure surfaces from kernel as a generic schema error; the specific
		// cause (e.g. an unsupported column type) is only on the builder.
		if (schema_builder.GetError().HasError()) {
			schema_builder.GetError().Throw();
		}
		builder_res.Throw();
	}

	if (!partition_columns.empty()) {
		vector<ffi::KernelStringSlice> slices;
		for (auto &partition_column : partition_columns) {
			slices.push_back(KernelUtils::ToDeltaString(partition_column));
		}
		// Consumes the builder handle unconditionally, including on error.
		auto partition_res =
		    KernelUtils::TryUnpackResult(ffi::create_table_txn_builder_with_partition_columns(
		                                     create_builder, slices.data(), slices.size(), engine.get()),
		                                 create_builder);
		if (partition_res.HasError()) {
			partition_res.Throw();
		}
	}

	for (auto &property : request.properties) {
		// Consumes the builder handle unconditionally, including on error.
		auto property_res = KernelUtils::TryUnpackResult(ffi::create_table_txn_builder_with_table_property(
		                                                     create_builder, KernelUtils::ToDeltaString(property.first),
		                                                     KernelUtils::ToDeltaString(property.second), engine.get()),
		                                                 create_builder);
		if (property_res.HasError()) {
			property_res.Throw();
		}
	}

	// A catalog-managed table's version 0 goes through the catalog's committer, as every commit after it
	// does: kernel refuses to create one through the filesystem committer.
	ffi::ExclusiveCreateTableTransaction *create_transaction;
	ErrorData build_res;
	if (committer) {
		ffi::MutableCommitter *catalog_committer;
		try {
			catalog_committer = committer();
		} catch (...) {
			ffi::free_create_table_txn_builder(create_builder);
			throw;
		}
		build_res = KernelUtils::TryUnpackResult(
		    ffi::create_table_txn_builder_build_with_committer(create_builder, engine.get(), catalog_committer),
		    create_transaction);
	} else {
		build_res = KernelUtils::TryUnpackResult(ffi::create_table_txn_builder_build(create_builder, engine.get()),
		                                         create_transaction);
	}
	if (build_res.HasError()) {
		build_res.Throw();
	}
	transaction = create_transaction;
}

DeltaCreateTableTransaction::~DeltaCreateTableTransaction() {
	if (transaction) {
		ffi::free_create_table_txn(transaction);
		transaction = nullptr;
	}
}

namespace {

struct PartitionNameVisitorState {
	vector<string> physical_names;
};

void VisitPartitionName(ffi::NullableCvoid engine_context, ffi::KernelStringSlice key, ffi::KernelStringSlice value,
                        bool is_null) {
	if (is_null) {
		return;
	}
	auto &state = *static_cast<PartitionNameVisitorState *>(const_cast<void *>(engine_context));
	state.physical_names.push_back(KernelUtils::FromDeltaString(key));
}

//! Kernel's "null of this type" tag, for a partition column that only has to be present in a partition value map
uint8_t NullTypeTag(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return 0;
	case LogicalTypeId::TINYINT:
		return 1;
	case LogicalTypeId::SMALLINT:
		return 2;
	case LogicalTypeId::INTEGER:
		return 3;
	case LogicalTypeId::BIGINT:
		return 4;
	case LogicalTypeId::FLOAT:
		return 5;
	case LogicalTypeId::DOUBLE:
		return 6;
	case LogicalTypeId::VARCHAR:
		return 7;
	case LogicalTypeId::BLOB:
		return 8;
	case LogicalTypeId::DATE:
		return 9;
	case LogicalTypeId::TIMESTAMP_TZ:
		return 10;
	case LogicalTypeId::TIMESTAMP:
		return 11;
	case LogicalTypeId::DECIMAL:
		return 12;
	default:
		throw NotImplementedException("Delta partition column of type %s is not supported", type.ToString());
	}
}

//! Inserts a non-null value of `type` under `name`; which value does not matter, only that it is not NULL
ffi::ExternResult<bool> InsertMarkerValue(ffi::ExclusivePartitionValueMap *map, const string &name,
                                          const LogicalType &type, ffi::SharedExternEngine *engine) {
	auto slice = KernelUtils::ToDeltaString(name);
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return ffi::partition_value_map_insert_bool(map, slice, false, engine);
	case LogicalTypeId::TINYINT:
		return ffi::partition_value_map_insert_byte(map, slice, 0, engine);
	case LogicalTypeId::SMALLINT:
		return ffi::partition_value_map_insert_short(map, slice, 0, engine);
	case LogicalTypeId::INTEGER:
		return ffi::partition_value_map_insert_int(map, slice, 0, engine);
	case LogicalTypeId::BIGINT:
		return ffi::partition_value_map_insert_long(map, slice, 0, engine);
	case LogicalTypeId::FLOAT:
		return ffi::partition_value_map_insert_float(map, slice, 0, engine);
	case LogicalTypeId::DOUBLE:
		return ffi::partition_value_map_insert_double(map, slice, 0, engine);
	case LogicalTypeId::VARCHAR: {
		const string marker = "0";
		return ffi::partition_value_map_insert_string(map, slice, KernelUtils::ToDeltaString(marker), engine);
	}
	case LogicalTypeId::BLOB: {
		const uint8_t marker = 0;
		return ffi::partition_value_map_insert_binary(map, slice, &marker, 1, engine);
	}
	case LogicalTypeId::DATE:
		return ffi::partition_value_map_insert_date(map, slice, 0, engine);
	case LogicalTypeId::TIMESTAMP_TZ:
		return ffi::partition_value_map_insert_timestamp(map, slice, 0, engine);
	case LogicalTypeId::TIMESTAMP:
		return ffi::partition_value_map_insert_timestamp_ntz(map, slice, 0, engine);
	case LogicalTypeId::DECIMAL:
		return ffi::partition_value_map_insert_decimal(map, slice, 0, 0, DecimalType::GetWidth(type),
		                                               DecimalType::GetScale(type), engine);
	default:
		throw NotImplementedException("Delta partition column of type %s is not supported", type.ToString());
	}
}

} // namespace

vector<DeltaMultiFileColumnDefinition> DeltaCreateTableTransaction::GetWriteColumns(ClientContext &context) {
	if (!transaction) {
		throw InternalException("DeltaCreateTableTransaction::GetWriteColumns after the transaction was consumed");
	}

	// A write context per set of partition values: so for each partition column, one where only that column is not
	// NULL, which then tells its physical name apart from the others'. Without partition columns, one context.
	// Kernel keys partition values by physical name and leaves partition columns out of the write schema.
	auto write_context_for = [&](optional_idx marked) {
		ffi::SharedWriteContext *write_context;
		ErrorData res;
		if (partition_columns.empty()) {
			res = KernelUtils::TryUnpackResult(
			    ffi::create_table_txn_get_unpartitioned_write_context(transaction, engine.get()), write_context);
		} else {
			auto map = ffi::partition_value_map_new();
			for (idx_t i = 0; i < partition_columns.size() && !res.HasError(); i++) {
				auto &type = partition_types[i];
				bool inserted;
				auto insert =
				    marked.IsValid() && marked.GetIndex() == i
				        ? InsertMarkerValue(map, partition_columns[i], type, engine.get())
				        : ffi::partition_value_map_insert_null(
				              map, KernelUtils::ToDeltaString(partition_columns[i]), NullTypeTag(type),
				              type.id() == LogicalTypeId::DECIMAL ? DecimalType::GetWidth(type) : 0,
				              type.id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(type) : 0, engine.get());
				res = KernelUtils::TryUnpackResult(insert, inserted);
			}
			if (res.HasError()) {
				ffi::free_partition_value_map(map);
				res.Throw();
			}
			// Consumes the map, on error too
			res = KernelUtils::TryUnpackResult(
			    ffi::create_table_txn_get_partitioned_write_context(transaction, map, engine.get()), write_context);
		}
		if (res.HasError()) {
			res.Throw();
		}
		return write_context;
	};

	auto write_context = write_context_for(optional_idx());
	auto schema = ffi::get_write_schema(write_context);
	vector<DeltaMultiFileColumnDefinition> data_columns;
	try {
		data_columns = KernelSchemaVisitor::ToColumnDefinitions(engine.get(), schema, DeltaColumnMappingMode::NONE);
	} catch (...) {
		ffi::free_schema(schema);
		ffi::free_write_context(write_context);
		throw;
	}
	ffi::free_schema(schema);
	ffi::free_write_context(write_context);

	bool mapped = false;
	for (auto &col : data_columns) {
		mapped |= col.IsColumnMapped();
	}

	vector<string> partition_physical_names;
	for (idx_t i = 0; i < partition_columns.size(); i++) {
		if (!mapped) {
			partition_physical_names.push_back(partition_columns[i]);
			continue;
		}
		auto marked_context = write_context_for(i);
		PartitionNameVisitorState state;
		ffi::visit_partition_values(marked_context, &state, VisitPartitionName);
		ffi::free_write_context(marked_context);
		if (state.physical_names.size() != 1) {
			throw InternalException("Delta create transaction reported %llu physical names for partition column '%s'",
			                        state.physical_names.size(), partition_columns[i]);
		}
		partition_physical_names.push_back(std::move(state.physical_names[0]));
	}

	// Back into table order: the data columns as kernel reports them, a partition column where the table has it
	vector<DeltaMultiFileColumnDefinition> result;
	idx_t next_data_column = 0;
	for (auto &column : columns.Logical()) {
		auto name = column.Name().GetIdentifierName();
		auto partition = std::find(partition_columns.begin(), partition_columns.end(), name);
		if (partition != partition_columns.end()) {
			DeltaMultiFileColumnDefinition partition_column(name, column.Type(), true);
			if (mapped) {
				partition_column.physical_name = partition_physical_names[partition - partition_columns.begin()];
			}
			result.push_back(std::move(partition_column));
			continue;
		}
		if (next_data_column >= data_columns.size() ||
		    data_columns[next_data_column].name.GetIdentifierName() != name) {
			throw InternalException("Delta create transaction's write schema does not list column '%s' in table order",
			                        name);
		}
		result.push_back(std::move(data_columns[next_data_column++]));
	}
	if (next_data_column != data_columns.size()) {
		throw InternalException("Delta create transaction's write schema has columns the table does not");
	}
	return result;
}

void DeltaCreateTableTransaction::AddFiles(ClientContext &context, const vector<DeltaDataFile> &files) {
	if (!transaction) {
		throw InternalException("DeltaCreateTableTransaction::AddFiles after the transaction was consumed");
	}
	if (files.empty()) {
		return;
	}
	auto engine_data = CreateAddFilesEngineData(context, Path::Normalize(path), files);
	ffi::create_table_txn_add_files(transaction, engine_data.release());
	added_files += files.size();
}

idx_t DeltaCreateTableTransaction::Commit(ClientContext &context) {
	if (!transaction) {
		throw InternalException("DeltaCreateTableTransaction::Commit after the transaction was consumed");
	}
	auto to_commit = transaction;
	transaction = nullptr;

	ffi::ExclusiveCommittedTransaction *committed;
	auto commit_res = KernelUtils::TryUnpackResult(ffi::create_table_txn_commit(to_commit, engine.get()), committed);
	if (commit_res.HasError()) {
		commit_res.Throw();
	}
	auto version = ffi::committed_transaction_version(&committed);
	ffi::free_committed_transaction(committed);
	DUCKDB_LOG_INTERNAL(context, "delta.CreateTable", LogLevel::LOG_DEBUG,
	                    "Created %s at version %s, adding %s files with data_change %s", path, to_string(version),
	                    to_string(added_files), data_change ? "true" : "false");
	return NumericCast<idx_t>(version);
}

} // namespace duckdb
