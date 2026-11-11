#include "storage/delta_catalog.hpp"
#include "storage/delta_schema_entry.hpp"
#include "storage/delta_transaction.hpp"
#include "storage/delta_create_table.hpp"
#include "storage/delta_insert.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include "functions/delta_scan/delta_multi_file_list.hpp"

namespace duckdb {

int64_t DeltaTimestampToEpochMs(timestamp_tz_t timestamp) {
	if (!timestamp.IsFinite()) {
		throw InvalidInputException("Delta time travel requires a finite timestamp");
	}
	return Timestamp::GetEpochMs(timestamp_t(timestamp));
}

string DeltaFormatEpochMs(int64_t timestamp_ms) {
	int64_t micros;
	if (!TryMultiplyOperator::Operation(timestamp_ms, Interval::MICROS_PER_MSEC, micros)) {
		return to_string(timestamp_ms) + "ms";
	}
	return Value::TIMESTAMPTZ(timestamp_tz_t(micros)).ToString();
}

// Rejects future timestamps, i.e. named > now(), where now() is defined as start of this transaction.
void DeltaRejectFutureTimestamp(ClientContext &context, int64_t timestamp_ms) {
	auto now = context.transaction.HasActiveTransaction()
	               ? MetaTransaction::Get(context).GetCurrentTransactionStartTimestamp()
	               : Timestamp::GetCurrentTimestamp();
	auto now_ms = Timestamp::GetEpochMs(now);
	if (timestamp_ms > now_ms) {
		throw InvalidInputException(
		    "Delta time travel does not accept future timestamp %s: that is later than now (%s)",
		    DeltaFormatEpochMs(timestamp_ms), DeltaFormatEpochMs(now_ms));
	}
}

DeltaTimeTravelSpec DeltaTimeTravelSpec::FromVersion(idx_t version) {
	DeltaTimeTravelSpec result;
	result.kind = Kind::VERSION;
	result.version = version;
	return result;
}

DeltaTimeTravelSpec DeltaTimeTravelSpec::FromTimestamp(timestamp_tz_t timestamp) {
	DeltaTimeTravelSpec result;
	result.kind = Kind::TIMESTAMP;
	result.timestamp = timestamp;
	return result;
}

idx_t DeltaTimeTravelSpec::GetVersion() const {
	if (!IsVersion()) {
		throw InternalException("DeltaTimeTravelSpec::GetVersion on a spec that does not name a version");
	}
	return version;
}

timestamp_tz_t DeltaTimeTravelSpec::GetTimestamp() const {
	if (!IsTimestamp()) {
		throw InternalException("DeltaTimeTravelSpec::GetTimestamp on a spec that does not name a timestamp");
	}
	return timestamp;
}

DeltaTimeTravelSpec DeltaTimeTravelSpec::FromAtClause(ClientContext &context, const BoundAtClause &at_clause) {
	auto &unit = at_clause.Unit();

	// Casting throws its own conversion error, which names the offending value and target type, so
	// wrapping it says nothing extra. This also matches how the ATTACH options read the same two units.
	if (unit == "version") {
		return FromVersion(at_clause.GetValue().DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>());
	}

	if (unit == "timestamp") {
		// Anything without a zone -- a naive TIMESTAMP or a string with no offset -- resolves through
		// the session timezone.
		return FromTimestamp(
		    at_clause.GetValue().CastAs(context, LogicalType::TIMESTAMP_TZ).GetValue<timestamp_tz_t>());
	}

	throw InvalidConfigurationException("Delta tables only support at_clause with unit 'version' or 'timestamp'");
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

ErrorData DeltaCatalog::SupportsCreateTable(BoundCreateTableInfo &info) {
	auto &base = info.Base().Cast<CreateTableInfo>();
	// Delta has partition columns and table properties, but nothing that SORTED BY maps onto.
	if (!base.sort_keys.empty()) {
		return ErrorData(ExceptionType::CATALOG,
		                 StringUtil::Format("SORTED BY is not supported for tables in a %s catalog", GetCatalogType()));
	}
	// DuckDB runs this before the schema's CreateTable, and also for a CREATE TABLE AS on a table it found, which it
	// may settle without that call. CREATE TABLE AS on an existing table plans a CREATE with the query's types, and
	// cannot be told apart from one here.
	try {
		ThrowIfDeltaCannotCreateTable(base, "Delta CREATE TABLE",
		                              "Declare the column as a type Delta can store, e.g. BIGINT or VARCHAR (in "
		                              "CREATE TABLE AS, cast it in the query, e.g. ::BIGINT).");
	} catch (std::exception &ex) {
		return ErrorData(ex);
	}
	return ErrorData();
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
	switch (if_not_found) {
	case OnEntryNotFound::RETURN_NULL:
		return nullptr;
	case OnEntryNotFound::THROW_EXCEPTION:
		throw CatalogException(schema_lookup.GetErrorContext(),
		                       "Schema \"%s\" does not exist! A delta catalog holds one table, under \"%s\"",
		                       schema_name, DEFAULT_SCHEMA);
	default:
		throw InternalException("Unknown OnEntryNotFound value %d", static_cast<int>(if_not_found));
	}
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
	// A catalog attached in child-catalog mode plans CTAS as a path table does: the parent (Unity Catalog) registers
	// the table after this statement's commit. One that commits through its parent (a catalog-managed table) builds
	// the create transaction on the parent's committer, as CREATE does.
	auto &base = op.info->Base();
	// What SupportsCreateTable refuses for a CREATE, before the options are bound and before any directory exists
	ThrowIfDeltaCannotCreateTable(
	    base, "Delta CREATE TABLE AS",
	    "Cast the column in the query to a type Delta can store, e.g. ::BIGINT or ::VARCHAR.");
	auto request = BindDeltaCreateTable(context, *this, base);
	DeltaCreateTableTransaction::CommitterFactory committer;
	if (parent_commit) {
		// Version 0 never calls back into the transaction: kernel's catalog committer writes it straight to the
		// published path, so the committer is made without a callback context
		auto &delta_transaction = DeltaTransaction::Get(context, *this);
		auto path = request.path;
		committer = [&delta_transaction, path]() {
			return delta_transaction.CreateCatalogCommitter(path, true);
		};
	}
	// The planner reaches here only when no table was found; IF NOT EXISTS on an existing one plans a plain CREATE,
	// which skips the query
	return DeltaInsert::PlanCreateTableAs(context, planner, plan, std::move(request), "Delta CREATE TABLE AS",
	                                      op.estimated_cardinality, committer);
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
