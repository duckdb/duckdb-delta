#include "storage/delta_catalog.hpp"
#include "storage/delta_schema_entry.hpp"
#include "storage/delta_transaction.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/main/attached_database.hpp"

#include "functions/delta_scan/delta_multi_file_list.hpp"

namespace duckdb {

idx_t ParseDeltaVersionFromAtClause(const BoundAtClause &at_clause) {
	if (StringUtil::Lower(at_clause.Unit()) != "version") {
		throw InvalidConfigurationException("Delta tables only support at_clause with unit 'version'");
	}
	Value version_value = at_clause.GetValue();
	if (!version_value.DefaultTryCastAs(LogicalType::UBIGINT, false)) {
		throw InvalidInputException("Failed to parse version number '%s' into a valid version",
		                            at_clause.GetValue().ToString().c_str());
	}
	return version_value.GetValue<idx_t>();
}

DeltaCatalog::DeltaCatalog(AttachedDatabase &db_p, const string &path, AccessMode access_mode)
    : Catalog(db_p), path(path), access_mode(access_mode), use_cache(false),
      use_specific_version(DConstants::INVALID_INDEX), pushdown_partition_info(true),
      filter_pushdown_mode(DEFAULT_PUSHDOWN_MODE) {
}

DeltaCatalog::~DeltaCatalog() = default;

void DeltaCatalog::Initialize(bool load_builtin) {
	CreateSchemaInfo info;
	main_schema = make_uniq<DeltaSchemaEntry>(*this, info);
}

optional_ptr<CatalogEntry> DeltaCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	throw BinderException("Delta tables do not support creating new schemas");
}

void DeltaCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw BinderException("Delta tables do not support dropping schemas");
}

void DeltaCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

optional_ptr<SchemaCatalogEntry> DeltaCatalog::LookupSchema(CatalogTransaction transaction,
                                                            const EntryLookupInfo &schema_lookup,
                                                            OnEntryNotFound if_not_found) {
	auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	return nullptr;
}

bool DeltaCatalog::InMemory() {
	return false;
}

string DeltaCatalog::GetDBPath() {
	return path;
}

bool DeltaCatalog::UseCachedSnapshot() {
	return use_cache;
}

optional_idx DeltaCatalog::GetCatalogVersion(ClientContext &context) {
	auto &delta_transaction = DeltaTransaction::Get(context, *this);

	// Option 1: snapshot is cached table-wide
	auto cached_snapshot = main_schema->GetCachedTable();
	if (cached_snapshot) {
		return cached_snapshot->snapshot->GetVersion();
	}

	// Option 2: snapshot is cached in transaction
	auto transaction_table_entry = delta_transaction.GetTableEntry(use_specific_version);
	if (transaction_table_entry) {
		return transaction_table_entry->snapshot->GetVersion();
	}

	return use_specific_version == DConstants::INVALID_INDEX ? optional_idx::Invalid() : use_specific_version;
}

DatabaseSize DeltaCatalog::GetDatabaseSize(ClientContext &context) {
	if (default_schema.empty()) {
		throw InvalidInputException("Attempting to fetch the database size - but no database was provided "
		                            "in the connection string");
	}
	DatabaseSize size;
	return size;
}

PhysicalOperator &DeltaCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                  LogicalCreateTable &op, PhysicalOperator &plan) {
	throw NotImplementedException("DeltaCatalog PlanCreateTableAs");
}
PhysicalOperator &DeltaCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                           PhysicalOperator &plan) {
	throw NotImplementedException("DeltaCatalog PlanDelete");
}
PhysicalOperator &DeltaCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                           PhysicalOperator &plan) {
	throw NotImplementedException("DeltaCatalog PlanUpdate");
}
unique_ptr<LogicalOperator> DeltaCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                          TableCatalogEntry &table, unique_ptr<LogicalOperator> plan) {
	throw NotImplementedException("DeltaCatalog BindCreateIndex");
}

} // namespace duckdb
