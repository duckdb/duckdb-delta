#include "storage/delta_schema_entry.hpp"

#include "delta_schema_builder.hpp"
#include "delta_utils.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "storage/delta_catalog.hpp"
#include "storage/delta_create_table.hpp"

#include "delta_extension.hpp"

#include "storage/delta_table_entry.hpp"
#include "storage/delta_transaction.hpp"

#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/table_function_binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"

namespace duckdb {

DeltaSchemaEntry::DeltaSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : SchemaCatalogEntry(catalog, info) {
}

DeltaSchemaEntry::~DeltaSchemaEntry() {
}

DeltaTransaction &GetDeltaTransaction(CatalogTransaction transaction) {
	if (!transaction.transaction) {
		throw InternalException("No transaction!?");
	}
	return transaction.transaction->Cast<DeltaTransaction>();
}

//! Applies DuckDB's CREATE conflict semantics ahead of the kernel call. Kernel remains the
//! authoritative existence check; this only decides what DuckDB should do when the table is already
//! there. Returns false when the statement should be skipped entirely.
static bool HandleCreateConflict(ClientContext &context, const CreateTableInfo &base, const string &path) {
	if (base.on_conflict != OnCreateConflict::IGNORE_ON_CONFLICT) {
		return true;
	}
	return !DeltaTableExistsAt(context, path);
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	if (!transaction.HasContext()) {
		throw NotImplementedException("Can not create a Delta table without context");
	}
	auto &context = transaction.GetContext();
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();
	auto &base = info.Base();

	auto request = BindDeltaCreateTable(context, delta_catalog, base);
	// Schema only: version 0 adds no files
	request.data_change = false;
	// Only what DuckDB can then INSERT; tables other engines made with nested VARIANT still read
	ThrowIfVariantIsNested(request.columns, "Delta CREATE TABLE");

	// After the options are read, so that a bad option fails even where IF NOT EXISTS makes the
	// statement a no-op: embedded SQL should not carry a typo until the day the table is gone.
	if (!HandleCreateConflict(context, base, request.path)) {
		return nullptr;
	}

	// After the conflict check, where the kernel call refused these before: CREATE TABLE AS IF NOT EXISTS on an
	// existing table plans this CREATE with the query's types, and stays a no-op. Before the directory exists.
	ThrowIfDeltaCannotStore(request.columns, "Delta CREATE TABLE",
	                        "Declare the column as a type Delta can store, e.g. BIGINT or VARCHAR.");
	// Only what DuckDB can then INSERT; tables other engines made with a LIST map key still read
	ThrowIfMapKeyIsList(request.columns, "Delta CREATE TABLE");

	auto created_directory = EnsureTableDirectory(context, request.path);

	// A catalog-managed table's version 0 goes through the catalog's committer. Kernel writes version 0
	// itself and leaves the table unregistered, so whoever staged it finishes the registration.
	DeltaCreateTableTransaction::CommitterFactory committer;
	if (delta_catalog.parent_commit) {
		auto &delta_transaction = GetDeltaTransaction(transaction);
		committer = [&]() {
			return delta_transaction.CreateCatalogCommitter(request.path);
		};
	}
	unique_ptr<DeltaCreateTableTransaction> create_transaction;
	try {
		create_transaction = make_uniq<DeltaCreateTableTransaction>(context, request, committer);
	} catch (std::exception &ex) {
		// Kernel's refusal says what is wrong; add what to do about it
		ErrorData error(ex);
		if (DeltaTableExistsAt(context, request.path)) {
			throw Exception(error.Type(),
			                error.RawMessage() +
			                    ". Delta tables cannot be replaced or dropped yet: use CREATE TABLE IF NOT "
			                    "EXISTS, or attach another path");
		}
		// Kernel refused the table (a property, a partition column): leave no directory behind for it
		if (created_directory) {
			try {
				auto &fs = FileSystem::GetFileSystem(context);
				bool empty = true;
				fs.ListFiles(request.path, [&](const string &, bool) { empty = false; });
				if (empty) {
					fs.RemoveDirectory(request.path);
				}
			} catch (...) {
			}
		}
		throw;
	}
	create_transaction->Commit(context);

	if (delta_catalog.parent_commit) {
		// A CMT cannot be read from storage until the catalog knows its commits, and the catalog learns about this once
		// it's registered; thus, there is nothing to serve here; that caller's own catalog answers the next lookup.
		return nullptr;
	}

	// Serve the table we just committed through the regular lookup path, so the schema cache and the
	// transaction's entry end up in the same state as for a table that already existed. Reading it back
	// also means the catalog serves the new snapshot without a re-attach.
	EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, QualifiedName(base.GetTableName()));
	return LookupEntry(transaction, lookup_info);
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) {
	throw BinderException("Delta tables do not support creating functions");
}

void DeltaUnqualifyColumnRef(ParsedExpression &expr) {
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		auto name = std::move(colref.ColumnNamesMutable().back());
		colref.ColumnNamesMutable() = {std::move(name)};
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(expr, DeltaUnqualifyColumnRef);
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                         TableCatalogEntry &table) {
	throw NotImplementedException("CreateIndex");
}

string GetDeltaCreateView(CreateViewInfo &info) {
	throw NotImplementedException("GetCreateView");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	throw BinderException("Delta tables do not support creating views");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	throw BinderException("Delta databases do not support creating types");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) {
	throw BinderException("Delta databases do not support creating sequences");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                 CreateTableFunctionInfo &info) {
	throw BinderException("Delta databases do not support creating table functions");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                                CreateCopyFunctionInfo &info) {
	throw BinderException("Delta databases do not support creating copy functions");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                  CreatePragmaFunctionInfo &info) {
	throw BinderException("Delta databases do not support creating pragma functions");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateCollation(CatalogTransaction transaction,
                                                             CreateCollationInfo &info) {
	throw BinderException("Delta databases do not support creating collations");
}

void DeltaSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	throw NotImplementedException("Delta tables do not support altering");
}

static bool CatalogTypeIsSupported(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
		return true;
	default:
		return false;
	}
}

shared_ptr<DeltaMultiFileList> DeltaSchemaEntry::CreateFileList(ClientContext &context, idx_t version,
                                                                optional_ptr<const DeltaMultiFileList> old_snapshot) {
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();
	auto snapshot = make_shared_ptr<DeltaMultiFileList>(context, delta_catalog.GetDBPath(), version, old_snapshot);

	// Set log_tail and max_catalog_version for catalog-managed commits (CCV2) if available
	if (!delta_catalog.catalog_log_tail.IsNull()) {
		snapshot->delta_log_path = make_uniq<DeltaLogPathArray>(delta_catalog.catalog_log_tail);
	}
	if (delta_catalog.max_catalog_version >= 0) {
		snapshot->max_catalog_version = delta_catalog.max_catalog_version;
	}

	return snapshot;
}

// req: this.lock must already be owned, since it reads cached_table
const DeltaMultiFileList &DeltaSchemaEntry::TransactionHead(ClientContext &context, DeltaTransaction &transaction) {
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();
	if (!delta_catalog.UseCachedSnapshot()) {
		// As a plain read does, so later transactions read only the commits after this one
		if (!cached_table) {
			cached_table = CreateTableEntry(context, DConstants::INVALID_INDEX, nullptr);
		}
	} else if (!delta_catalog.has_specific_timestamp &&
	           delta_catalog.use_specific_version == DConstants::INVALID_INDEX) {
		// pin_snapshot: one latest version for every transaction
		if (!cached_table) {
			cached_table = CreateTableEntry(context, DConstants::INVALID_INDEX, nullptr);
		}
		return *cached_table->snapshot;
	}
	optional_ptr<const DeltaMultiFileList> old_snapshot;
	if (cached_table) {
		old_snapshot = cached_table->snapshot.get();
	}
	return *transaction.InitializeTableEntry(context, *this, DConstants::INVALID_INDEX, old_snapshot).snapshot;
}

// req: this.lock must already be owned
idx_t DeltaSchemaEntry::ResolveTimestamp(ClientContext &context, DeltaTransaction &transaction,
                                         timestamp_tz_t timestamp) {
	return TransactionHead(context, transaction).ResolveTimestampWithin(context, timestamp);
}

unique_ptr<DeltaTableEntry> DeltaSchemaEntry::CreateTableEntry(ClientContext &context, idx_t version,
                                                               optional_ptr<const DeltaMultiFileList> old_snapshot) {
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();
	auto snapshot = CreateFileList(context, version, old_snapshot);

	// Get the names and types from the delta snapshot
	vector<LogicalType> return_types;
	vector<Identifier> names;
	snapshot->Bind(return_types, names);

	// TODO: forward nullability constraints

	CreateTableInfo table_info;
	for (idx_t i = 0; i < return_types.size(); i++) {
		table_info.columns.AddColumn(ColumnDefinition(Identifier(names[i]), return_types[i]));
	}
	table_info.SetTableName(!delta_catalog.internal_table_name.empty() ? Identifier(delta_catalog.internal_table_name)
	                                                                   : catalog.GetName());

	// Copy over constraints to table info TODO: these are incompatible currently
	// table_info.constraints = snapshot->not_null_constraints;}

	// Populate tags from domain metadata
	{
		auto snapshot_ref = snapshot->snapshot->GetLockingRef();
		ffi::visit_domain_metadata(
		    snapshot_ref.GetPtr(), snapshot->extern_engine.get(), &table_info.tags,
		    [](ffi::NullableCvoid engine_context, ffi::KernelStringSlice domain, ffi::KernelStringSlice configuration) {
			    auto &tags = *static_cast<InsertionOrderPreservingMap<string> *>(const_cast<void *>(engine_context));
			    tags.insert({KernelUtils::FromDeltaString(domain), KernelUtils::FromDeltaString(configuration)});
		    });
	}

	auto table_entry = make_uniq<DeltaTableEntry>(delta_catalog, *this, table_info);
	table_entry->snapshot = std::move(snapshot);

	return table_entry;
}

void DeltaSchemaEntry::Scan(ClientContext &context, CatalogType type,
                            const std::function<void(CatalogEntry &)> &callback) {
	if (CatalogTypeIsSupported(type)) {
		auto transaction = catalog.GetCatalogTransaction(context);
		auto lookup_info = EntryLookupInfo(type, catalog.GetName());
		auto default_table = LookupEntry(transaction, lookup_info);
		if (default_table) {
			callback(*default_table);
		}
	}
}

void DeltaSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	throw NotImplementedException("Scan without context not supported");
}

void DeltaSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	throw NotImplementedException("Delta tables do not support dropping");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                         const EntryLookupInfo &lookup_info) {
	if (!transaction.HasContext()) {
		throw NotImplementedException("Can not DeltaSchemaEntry::GetEntry without context");
	}
	auto &context = transaction.GetContext();

	auto type = lookup_info.GetCatalogType();
	auto &name = lookup_info.GetEntryName();
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();

	if (type == CatalogType::TABLE_ENTRY && (name == catalog.GetName() || name == delta_catalog.internal_table_name)) {
		auto &delta_transaction = GetDeltaTransaction(transaction);

		// An attached timestamp binds to a version on first use; from there it is an attached version.
		// TODO: bind at ATTACH instead, once ATTACH stops deferring its work -- deferring here means an
		// unreadable table or an out-of-range timestamp is reported by the first query, not by ATTACH.
		if (delta_catalog.has_specific_timestamp && delta_catalog.use_specific_version == DConstants::INVALID_INDEX) {
			unique_lock<mutex> l(lock);
			if (delta_catalog.use_specific_version == DConstants::INVALID_INDEX) {
				delta_catalog.use_specific_version =
				    ResolveTimestamp(context, delta_transaction, delta_catalog.specific_timestamp);
			}
		}

		idx_t version = delta_catalog.use_specific_version;

		// If there's an AT clause we are doing timetravel. An attached version or timestamp is only the
		// default, so the AT clause overrides it.
		auto at_clause = lookup_info.GetAtClause();
		if (at_clause) {
			auto spec = DeltaTimeTravelSpec::FromAtClause(context, *at_clause);
			if (spec.IsTimestamp()) {
				auto timestamp = spec.GetTimestamp();
				auto resolved = delta_transaction.GetTimestampVersion(timestamp);
				if (resolved.IsValid()) {
					version = resolved.GetIndex();
					DUCKDB_LOG_INTERNAL(
					    context, "delta.TimeTravel", LogLevel::LOG_DEBUG,
					    "Timestamp %s reuses version %s, resolved earlier in this transaction, for '%s'",
					    Value::TIMESTAMPTZ(timestamp).ToString(), to_string(version),
					    DeltaMultiFileList::ToDeltaPath(delta_catalog.GetDBPath()));
				} else {
					unique_lock<mutex> l(lock);
					version = delta_transaction.SetTimestampVersion(
					    timestamp, ResolveTimestamp(context, delta_transaction, timestamp));
				}
			} else {
				version = spec.GetVersion();
			}
		}

		auto transaction_table_entry = delta_transaction.GetTableEntry(version);
		if (transaction_table_entry) {
			return *transaction_table_entry;
		}

		// With nothing cached the entry has to come from a snapshot, and kernel refuses to build one
		// where no table exists. Report that as "not found" instead of letting the IO error escape:
		// DuckDB looks the table up before CREATE TABLE, so without this no table can ever be created
		// at a fresh path, and plain catalog enumeration of an empty attach throws too.
		if (!GetCachedTable() && !DeltaTableExistsAt(context, delta_catalog.GetDBPath())) {
			return nullptr;
		}

		if (delta_catalog.UseCachedSnapshot()) {
			unique_lock<mutex> l(lock);

			// If the version being requested is different from the one we have cached, we
			if (delta_catalog.use_specific_version != version) {
				return delta_transaction.InitializeTableEntry(context, *this, version, nullptr);
			}

			if (!cached_table) {
				cached_table = CreateTableEntry(context, version, nullptr);
			}
			return *cached_table;
		} else {
			unique_lock<mutex> l(lock);

			if (!cached_table) {
				cached_table = CreateTableEntry(context, version, nullptr);
			}

			// Always go through InitializeTableEntry so the transaction's table_entry is set,
			// using the cached snapshot as base for fast re-initialization.
			return delta_transaction.InitializeTableEntry(context, *this, version, *cached_table->snapshot);
		}
	}
	return nullptr;
}

optional_ptr<DeltaTableEntry> DeltaSchemaEntry::GetCachedTable() {
	lock_guard<mutex> lck(lock);
	if (cached_table) {
		return *cached_table;
	}
	return nullptr;
}

} // namespace duckdb
