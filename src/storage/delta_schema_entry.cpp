#include "storage/delta_schema_entry.hpp"

#include "delta_schema_builder.hpp"
#include "delta_utils.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "storage/delta_catalog.hpp"

#include "delta_extension.hpp"

#include "storage/delta_table_entry.hpp"
#include "storage/delta_transaction.hpp"

#include "duckdb/common/to_string.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
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

//! Resolves the destination path for a CREATE TABLE. Defaults to the attached path; `WITH (path =
//! '...')` names it explicitly. Only constants are accepted -- the binder does not evaluate table
//! options, they arrive as raw parsed expressions.
static string GetCreateTablePath(const CreateTableInfo &base, DeltaCatalog &delta_catalog) {
	auto option = base.options.find("path");
	if (option == base.options.end()) {
		return delta_catalog.GetDBPath();
	}
	if (option->second->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw BinderException("Delta CREATE TABLE option 'path' must be a constant, found '%s'",
		                      option->second->ToString());
	}
	auto path = option->second->Cast<ConstantExpression>().GetValue().ToString();

	// A Delta catalog is a single table at a single path, so a divergent path would produce a
	// catalog entry that does not describe what was attached.
	if (path != delta_catalog.GetDBPath()) {
		throw NotImplementedException(
		    "Delta CREATE TABLE can only create a table at the attached path ('%s'), not at '%s'",
		    delta_catalog.GetDBPath(), path);
	}
	return path;
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

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	if (!transaction.HasContext()) {
		throw NotImplementedException("Can not create a Delta table without context");
	}
	auto &context = transaction.GetContext();
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();
	auto &base = info.Base();

	if (delta_catalog.access_mode == AccessMode::READ_ONLY) {
		throw InvalidInputException("Can not create a table in a read only Delta catalog");
	}

	// LookupEntry only ever resolves the single attached table, so any other name would create a
	// table that could never be read back.
	auto table_name = base.GetTableName().GetIdentifierName();
	if (table_name != catalog.GetName() && table_name != delta_catalog.internal_table_name) {
		throw NotImplementedException("Delta CREATE TABLE must use the attached name ('%s'), found '%s'",
		                              catalog.GetName().GetIdentifierName(), table_name);
	}
	if (!base.sort_keys.empty()) {
		throw NotImplementedException("Delta CREATE TABLE does not support SORTED BY");
	}
	for (auto &constraint : base.constraints) {
		// Delta requires columns to be nullable unless the `invariants` writer feature is enabled,
		// which we do not request yet.
		if (constraint->type == ConstraintType::NOT_NULL) {
			throw NotImplementedException("Delta CREATE TABLE does not support NOT NULL constraints");
		}
		throw NotImplementedException("Delta CREATE TABLE does not support constraints");
	}

	auto path = GetCreateTablePath(base, delta_catalog);
	auto partition_columns = GetCreateTablePartitionColumns(base);

	auto engine = CreateDeltaEngine(context, path);

	// The builder holds a borrowed pointer to `schema_builder`, and kernel runs the visitor during
	// get_create_table_builder -- both must outlive that call.
	DeltaSchemaBuilder schema_builder(base.columns);
	auto engine_schema = schema_builder.CreateEngineSchema();

	ffi::ExclusiveCreateTableBuilder *create_builder;
	auto builder_res = KernelUtils::TryUnpackResult(
	    ffi::get_create_table_builder(KernelUtils::ToDeltaString(path), &engine_schema,
	                                  KernelUtils::ToDeltaString("DuckDB"), engine.get()),
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
		auto partition_res = KernelUtils::TryUnpackResult(
		    ffi::create_table_builder_with_partition_columns(create_builder, slices.data(), slices.size(), engine.get()),
		    create_builder);
		if (partition_res.HasError()) {
			partition_res.Throw();
		}
	}

	ffi::ExclusiveCreateTransaction *create_transaction;
	auto build_res = KernelUtils::TryUnpackResult(ffi::create_table_builder_build(create_builder, engine.get()),
	                                              create_transaction);
	if (build_res.HasError()) {
		build_res.Throw();
	}

	ffi::ExclusiveCommittedTransaction *committed;
	auto commit_res =
	    KernelUtils::TryUnpackResult(ffi::create_table_commit(create_transaction, engine.get()), committed);
	if (commit_res.HasError()) {
		commit_res.Throw();
	}
	auto version = ffi::committed_transaction_version(&committed);
	ffi::free_committed_transaction(committed);
	DUCKDB_LOG_INTERNAL(context, "delta.CreateTable", LogLevel::LOG_DEBUG, "Created %s at version %s", path,
	                    to_string(version));

	// Re-read the table we just committed so the catalog serves the new snapshot without a re-attach.
	unique_lock<mutex> l(lock);
	cached_table = CreateTableEntry(context, DConstants::INVALID_INDEX, nullptr);
	return *cached_table;
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

unique_ptr<DeltaTableEntry> DeltaSchemaEntry::CreateTableEntry(ClientContext &context, idx_t version,
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

		idx_t version = delta_catalog.use_specific_version;

		// If there's an AT clause we are doing timetravel
		auto at_clause = lookup_info.GetAtClause();
		if (at_clause) {
			version = ParseDeltaVersionFromAtClause(*at_clause);
		}

		auto transaction_table_entry = delta_transaction.GetTableEntry(version);
		if (transaction_table_entry) {
			return *transaction_table_entry;
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
