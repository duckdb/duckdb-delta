#include "functions/delta_scan/delta_scan.hpp"
#include "storage/delta_catalog.hpp"
#include "storage/delta_table_entry.hpp"
#include "storage/delta_transaction.hpp"

#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

namespace duckdb {

DeltaTableEntry::DeltaTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
    : TableCatalogEntry(catalog, schema, info) {
	this->internal = false;
}

DeltaTableEntry::~DeltaTableEntry() = default;

unique_ptr<BaseStatistics> DeltaTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

void DeltaTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &, LogicalProjection &, LogicalUpdate &,
                                            ClientContext &) {
	throw NotImplementedException("BindUpdateConstraints for delta table");
}

TableFunction DeltaTableEntry::GetScanFunctionInternal(ClientContext &context, unique_ptr<FunctionData> &bind_data,
                                                       optional_ptr<const EntryLookupInfo> lookup_info) {
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &system_catalog = Catalog::GetSystemCatalog(db);

	auto data = CatalogTransaction::GetSystemTransaction(db);
	auto &schema = system_catalog.GetSchema(data, Identifier::DefaultSchema());
	auto catalog_entry = schema.GetEntry(data, CatalogType::TABLE_FUNCTION_ENTRY, "delta_scan");
	if (!catalog_entry) {
		throw InvalidInputException("Function with name \"%s\" not found in ExtensionLoader::GetTableFunction", name);
	}
	auto &delta_function_set = catalog_entry->Cast<TableFunctionCatalogEntry>();

	// copied out of the set: the bind mutates function_info and needs a mutable function
	auto delta_scan_function = *delta_function_set.functions.GetFunctionByArguments(context, {LogicalType::VARCHAR});
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();

	auto &transaction = DeltaTransaction::Get(context, delta_catalog);
	if (transaction.HasOutstandingAppends()) {
		throw CatalogException("Scanning a table with uncommitted writes is not supported");
	}

	// Copy over the internal kernel snapshot
	auto function_info = make_shared_ptr<DeltaFunctionInfo>();

	idx_t version = DConstants::INVALID_INDEX;
	if (lookup_info && lookup_info->GetAtClause()) {
		auto spec = DeltaTimeTravelSpec::FromAtClause(*lookup_info->GetAtClause());
		// A timestamp was already bound to a version during the catalog lookup that produced this
		// entry; resolving it again would re-read the log and could land on a newer commit.
		version = spec.IsTimestamp() ? snapshot->GetVersion() : spec.GetVersion();
	}

	if (version != DConstants::INVALID_INDEX && snapshot->GetVersion() != version) {
		throw InternalException("Delta table snapshot version does not match at clause version.");
	}

	function_info->snapshot = this->snapshot;
	function_info->table_name = delta_catalog.GetName().GetIdentifierName();
	delta_scan_function.function_info = std::move(function_info);

	vector<Value> inputs = {delta_catalog.GetDBPath()};
	named_parameter_map_t param_map;
	vector<LogicalType> return_types;
	vector<Identifier> names;
	TableFunctionRef empty_ref;

	// Propagate settings
	param_map.insert({"pushdown_partition_info", delta_catalog.pushdown_partition_info});
	param_map.insert({"pushdown_filters", DeltaEnumUtils::ToString(delta_catalog.filter_pushdown_mode)});
	idx_t param_version = version != DConstants::INVALID_INDEX ? version : delta_catalog.use_specific_version;
	if (param_version != DConstants::INVALID_INDEX) {
		param_map.insert({"version", Value::UBIGINT(param_version)});
	}

	TableFunctionBindInput bind_input(inputs, param_map, return_types, names, nullptr, nullptr, delta_scan_function,
	                                  empty_ref);

	auto result = delta_scan_function.bind(context, bind_input, return_types, names);
	bind_data = std::move(result);

	return delta_scan_function;
}

case_insensitive_map_t<vector<NestedNotNullConstraint>> DeltaTableEntry::GetNotNullConstraints() const {
	case_insensitive_map_t<vector<NestedNotNullConstraint>> result;
	for (auto &constraint : snapshot->GetNestedNotNullConstraints()) {
		auto &col = GetColumn(constraint.index);
		auto &item = result[col.Name().GetIdentifierName()];
		item.push_back(constraint);
	}
	return result;
}

void DeltaTableEntry::ThrowOnUnsupportedFieldForInserting() const {
	if (!snapshot) {
		return;
	}
	if (snapshot->HasNullConstraintsInArrays()) {
		throw NotImplementedException("Inserting into a table with null constraints in arrays is not supported");
	}

	// Column mapping addresses parquet columns by physical name and field id. We supply both for top-level
	// columns only, so the cases below would produce a file that is mapped in part -- which reads back as nulls
	// for whatever went unmapped, and in id mode is worse than writing nothing mapped at all: a file carrying
	// some field ids no longer trips the reader's "no field ids, refuse it" rule, so partial loss passes silently.
	bool column_mapped = false;
	bool mapped_nested = false;
	for (auto &col : snapshot->GetLazyLoadedGlobalColumns()) {
		if (col.physical_name.empty() && !col.field_id.IsValid()) {
			continue;
		}
		column_mapped = true;
		if (!col.children.empty()) {
			mapped_nested = true;
		}
	}
	if (!column_mapped) {
		return;
	}
	if (mapped_nested) {
		throw NotImplementedException(
		    "Inserting into a Delta table that uses column mapping on a nested column is not supported");
	}
	if (!snapshot->GetPartitionColumns().empty()) {
		throw NotImplementedException(
		    "Inserting into a partitioned Delta table that uses column mapping is not supported");
	}
}

TableFunction DeltaTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
                                               const EntryLookupInfo &lookup_info) {
	return GetScanFunctionInternal(context, bind_data, lookup_info);
}

TableFunction DeltaTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	return GetScanFunctionInternal(context, bind_data, nullptr);
}

TableStorageInfo DeltaTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	// TODO fill info
	return result;
}

} // namespace duckdb
