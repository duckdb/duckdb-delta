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
#include "duckdb/common/type_visitor.hpp"
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

//! Every option is evaluated to a string; `location` then names the destination (TakeLocationProperty), and every
//! other option is a Delta table property, handed to kernel untouched: kernel decides which keys it recognizes and
//! derives the protocol from them.
static vector<pair<string, string>> GetCreateTableOptions(ClientContext &context, TableFunctionBinder &binder,
                                                          const CreateTableInfo &base) {
	vector<pair<string, string>> options;
	for (auto &option : base.options) {
		auto value = ParseCreateTableOption(context, binder, *option.second, option.first, LogicalType::VARCHAR);
		options.emplace_back(option.first, value.GetValue<string>());
	}
	return options;
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

void ThrowIfPartitionTypeUnwritable(const string &column_name, const LogicalType &type, const string &statement) {
	if (type.id() != LogicalTypeId::BLOB) {
		return;
	}
	// The log keeps partition values as JSON strings. Kernel's writer refuses bytes that are not UTF-8, Spark turns
	// them into U+FFFD, and readers take the string's UTF-8 bytes back, so most BLOB values cannot be written and read
	// back as they were.
	throw NotImplementedException("%s does not support partitioning on column '%s' of type BLOB: Delta stores "
	                              "partition values as text, which cannot hold arbitrary bytes. Partition on another "
	                              "column, or on a VARCHAR column holding a text form of it, e.g. hex(%s).",
	                              statement, column_name, column_name);
}

vector<string> ResolvePartitionColumns(const ColumnList &columns, const vector<string> &keys, const string &statement,
                                       const string &clause) {
	vector<string> result;
	for (auto &key : keys) {
		optional_ptr<const ColumnDefinition> match;
		for (auto &column : columns.Logical()) {
			if (StringUtil::CIEquals(column.Name().GetIdentifierName(), key)) {
				match = &column;
				break;
			}
		}
		if (!match) {
			throw BinderException("%s: %s names column '%s', which the table does not have", statement, clause, key);
		}
		auto name = match->Name().GetIdentifierName();
		ThrowIfPartitionTypeUnwritable(name, match->Type(), statement);
		if (std::find(result.begin(), result.end(), name) != result.end()) {
			throw BinderException("%s: %s names column '%s' twice", statement, clause, name);
		}
		result.push_back(std::move(name));
	}
	return result;
}

void TakeLocationProperty(vector<pair<string, string>> &properties, const string &target, const string &target_name,
                          const string &statement) {
	// `path` named the destination before `location` did, and never shipped, so it is refused rather than taken for a
	// table property. `location` is spelled as the Iceberg catalog spells it.
	bool has_location = false;
	for (idx_t i = 0; i < properties.size(); i++) {
		auto &key = properties[i].first;
		if (StringUtil::CIEquals(key, "path")) {
			throw BinderException("%s names the destination with 'location', not 'path'", statement);
		}
		if (!StringUtil::CIEquals(key, "location")) {
			continue;
		}
		if (has_location) {
			throw BinderException("%s: 'location' is given twice", statement);
		}
		has_location = true;
		// A Delta table lives at one path, so a location elsewhere would make a table that the statement does not
		// describe. Compared normalized, so a trailing slash or a relative spelling is not a spurious mismatch.
		if (CanonicalTablePath(properties[i].second) != CanonicalTablePath(target)) {
			throw NotImplementedException("%s can only create a table at %s ('%s'), not at '%s'", statement,
			                              target_name, target, properties[i].second);
		}
		properties.erase(properties.begin() + NumericCast<int64_t>(i));
		i--;
	}
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
	request.path = delta_catalog.GetDBPath();
	request.partition_columns = ResolvePartitionColumns(base.columns, GetCreateTablePartitionColumns(base),
	                                                    "Delta CREATE TABLE", "PARTITIONED BY");
	request.properties = GetCreateTableOptions(context, option_binder, base);
	TakeLocationProperty(request.properties, request.path, "the attached path", "Delta CREATE TABLE");
	// A `delta.*` key must be one Delta has, spelled exactly; any other key is the user's own property, kept as given
	for (auto &property : request.properties) {
		DeltaTableProperties::ThrowIfUnknownDeltaKey(property.first, "Delta CREATE TABLE");
	}
	// Sorted so that two bad properties always fail in the same order; kernel orders the written configuration
	std::sort(request.properties.begin(), request.properties.end());
	request.columns = base.columns.Copy();

	if (base.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		// Replacing means dropping, and Delta tables do not support dropping yet.
		throw NotImplementedException("Delta tables do not support CREATE OR REPLACE TABLE");
	}
	return request;
}

void ThrowIfDeltaCannotStore(const ColumnList &columns, const string &statement, const string &hint) {
	for (auto &column : columns.Logical()) {
		auto &type = column.Type();
		auto unstorable = DeltaSchemaBuilder::FindUnsupportedType(type);
		if (!unstorable) {
			continue;
		}
		auto nested =
		    unstorable.get() == &type ? string() : StringUtil::Format(" (it holds a %s)", unstorable->ToString());
		throw NotImplementedException("%s does not support column '%s' of type %s%s: Delta has no such type. %s",
		                              statement, column.Name().GetIdentifierName(), type.ToString(), nested, hint);
	}
}

void ThrowIfVariantIsNested(const ColumnList &columns, const string &statement) {
	for (auto &column : columns.Logical()) {
		auto &type = column.Type();
		if (type.id() != LogicalTypeId::VARIANT && TypeVisitor::Contains(type, LogicalTypeId::VARIANT)) {
			throw NotImplementedException("%s does not support column '%s' with a VARIANT nested in a %s "
			                              "yet; only top-level VARIANT columns can be written",
			                              statement, column.Name().GetIdentifierName(),
			                              LogicalTypeIdToString(type.id()));
		}
	}
}

//! Whether `type` holds a MAP whose key is a LIST, at any depth
static bool HoldsMapWithListKey(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			if (HoldsMapWithListKey(child.second)) {
				return true;
			}
		}
		return false;
	case LogicalTypeId::LIST:
		return HoldsMapWithListKey(ListType::GetChildType(type));
	case LogicalTypeId::MAP: {
		auto &key = MapType::KeyType(type);
		return key.id() == LogicalTypeId::LIST || HoldsMapWithListKey(key) ||
		       HoldsMapWithListKey(MapType::ValueType(type));
	}
	default:
		return false;
	}
}

void ThrowIfMapKeyIsList(const ColumnList &columns, const string &statement) {
	for (auto &column : columns.Logical()) {
		if (HoldsMapWithListKey(column.Type())) {
			throw NotImplementedException("%s does not support column '%s' of type %s: a MAP with a LIST key is a "
			                              "non-nullable list in Delta, which INSERT cannot write yet",
			                              statement, column.Name().GetIdentifierName(), column.Type().ToString());
		}
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

void TryRemoveEmptyDirectory(FileSystem &fs, const string &directory) {
	try {
		fs.RemoveDirectoryExtended(directory, {RemoveDirectoryMode::SINGLE});
	} catch (std::exception &) {
	}
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

	// A write context needs a value for every partition column. The base one (all NULL) gives the data columns; one
	// per partition column, with only that column not NULL, tells its physical name apart from the others', since the
	// context reports partition values under physical names (the map takes them under logical ones).
	auto write_context_for = [&](optional_idx marked) {
		ffi::SharedWriteContext *write_context;
		ErrorData res;
		if (partition_columns.empty()) {
			res = KernelUtils::TryUnpackResult(
			    ffi::create_table_txn_get_unpartitioned_write_context(transaction, engine.get()), write_context);
		} else {
			auto map = ffi::partition_value_map_new();
			// Freed here on every way out of the loop, a throw included; the write context call below consumes it
			struct MapGuard {
				ffi::ExclusivePartitionValueMap *map;
				~MapGuard() {
					if (map) {
						ffi::free_partition_value_map(map);
					}
				}
			} guard {map};
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
				res.Throw();
			}
			guard.map = nullptr;
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
	// The logical write schema has the data columns with their physical names and ids in the column mapping
	// metadata. The physical one names the fields physically, with `parquet.field.id`, and also holds the partition
	// columns when the table's features require them in the files (materializePartitionColumns, icebergCompatV2/V3).
	auto schema = ffi::get_write_schema(write_context);
	auto physical_schema = ffi::get_physical_write_schema(write_context);
	vector<DeltaMultiFileColumnDefinition> data_columns;
	vector<DeltaMultiFileColumnDefinition> physical_columns;
	try {
		data_columns = KernelSchemaVisitor::ToColumnDefinitions(engine.get(), schema, DeltaColumnMappingMode::NONE);
		physical_columns =
		    KernelSchemaVisitor::ToColumnDefinitions(engine.get(), physical_schema, DeltaColumnMappingMode::NONE);
	} catch (...) {
		ffi::free_schema(schema);
		ffi::free_schema(physical_schema);
		ffi::free_write_context(write_context);
		throw;
	}
	ffi::free_schema(schema);
	ffi::free_schema(physical_schema);
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

	// A partition column the files must hold: the physical schema lists it, with its field id
	idx_t materialized = 0;
	vector<optional_ptr<const DeltaMultiFileColumnDefinition>> partition_physical(partition_columns.size());
	for (idx_t i = 0; i < partition_columns.size(); i++) {
		for (auto &physical : physical_columns) {
			if (physical.name.GetIdentifierName() == partition_physical_names[i]) {
				partition_physical[i] = &physical;
				materialized++;
				break;
			}
		}
	}
	if (materialized != 0 && materialized != partition_columns.size()) {
		throw InternalException("Delta create transaction's physical schema holds %llu of the %llu partition columns",
		                        materialized, partition_columns.size());
	}
	partition_columns_materialized = !partition_columns.empty() && materialized == partition_columns.size();

	// Back into table order: the data columns as kernel reports them, a partition column where the table has it
	vector<DeltaMultiFileColumnDefinition> result;
	idx_t next_data_column = 0;
	for (auto &column : columns.Logical()) {
		auto name = column.Name().GetIdentifierName();
		auto partition = std::find(partition_columns.begin(), partition_columns.end(), name);
		if (partition != partition_columns.end()) {
			auto index = NumericCast<idx_t>(partition - partition_columns.begin());
			DeltaMultiFileColumnDefinition partition_column(name, column.Type(), true);
			if (mapped) {
				partition_column.physical_name = partition_physical_names[index];
				if (partition_physical[index]) {
					partition_column.field_id = partition_physical[index]->field_id;
				}
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
