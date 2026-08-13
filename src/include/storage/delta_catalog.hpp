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

//! A time travel target as written by the user: either a version, or a timestamp that still has to
//! be resolved into one. Once resolved, a timestamp is a version like any other.
struct DeltaTimeTravelSpec {
	static DeltaTimeTravelSpec FromAtClause(const BoundAtClause &at_clause);

	bool IsTimestamp() const {
		return is_timestamp;
	}

	//! Only valid when !IsTimestamp()
	idx_t version = DConstants::INVALID_INDEX;
	//! Only valid when IsTimestamp()
	timestamp_tz_t timestamp = timestamp_tz_t(0);

private:
	bool is_timestamp = false;
};

//! Milliseconds since the unix epoch, which is how the delta protocol spells timestamps
int64_t DeltaTimestampToEpochMs(timestamp_tz_t timestamp);

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
	//! Time travel target from `ATTACH ... (TIMESTAMP => ...)`. Resolved into use_specific_version on
	//! the first table lookup, since resolving needs a snapshot and ATTACH must not read the log.
	bool has_specific_timestamp = false;
	timestamp_tz_t specific_timestamp = timestamp_tz_t(0);
	bool pushdown_partition_info;
	DeltaFilterPushdownMode filter_pushdown_mode;

	string internal_table_name;
	bool child_catalog_mode = false;
	string parent_catalog_name;
	optional_ptr<TableCatalogEntry> parent_table_entry;
	bool parent_commit = false;
	optional_ptr<TableFunctionCatalogEntry> commit_function;
	string unity_table_id;

	// Store the log_tail and max_catalog_version for CMTs
	Value catalog_log_tail;
	int64_t max_catalog_version = -1;

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
