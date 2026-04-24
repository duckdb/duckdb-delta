//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_catalog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "functions/delta_scan/delta_scan.hpp"
#include "delta_schema_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/enums/access_mode.hpp"

namespace duckdb {
class DeltaSchemaEntry;

idx_t ParseDeltaVersionFromAtClause(const BoundAtClause &at_clause);

class DeltaClearCacheFunction : public TableFunction {
public:
	DeltaClearCacheFunction();

	static void ClearCacheOnSetting(ClientContext &context, SetScope scope, Value &parameter);
};

class DeltaCatalog : public Catalog {
public:
	explicit DeltaCatalog(AttachedDatabase &db_p, const string &path, AccessMode access_mode);
	~DeltaCatalog();

	string path;
	AccessMode access_mode;
	bool use_cache;
	idx_t use_specific_version;
	bool pushdown_partition_info;
	DeltaFilterPushdownMode filter_pushdown_mode;

	string internal_table_name;
	bool child_catalog_mode = false;
	string parent_catalog_name;
	optional_ptr<TableCatalogEntry> parent_table_entry;
	bool parent_commit = false;
	optional_ptr<TableFunctionCatalogEntry> commit_function;
	string unity_table_id;

	// Store the log_tail for catalog-managed commits (CCV2)
	Value catalog_log_tail;

public:
	string GetInternalTableName() {
		return internal_table_name;
	}

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return "delta";
	}

	bool SupportsTimeTravel() const override {
		return true;
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;

	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;

	optional_idx GetCatalogVersion(ClientContext &context) override;

	bool InMemory() override;
	string GetDBPath() override;

	bool UseCachedSnapshot();

	DeltaSchemaEntry &GetMainSchema() {
		return *main_schema;
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	unique_ptr<DeltaSchemaEntry> main_schema;
	string default_schema;
};

} // namespace duckdb
