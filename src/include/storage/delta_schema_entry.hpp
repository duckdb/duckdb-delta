//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_schema_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "storage/delta_table_entry.hpp"

namespace duckdb {
class DeltaTransaction;
class DeltaCatalog;

class DeltaSchemaEntry : public SchemaCatalogEntry {
public:
	DeltaSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);
	~DeltaSchemaEntry() override;

public:
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	optional_ptr<DeltaTableEntry> GetCachedTable();

	unique_ptr<DeltaTableEntry> CreateTableEntry(ClientContext &context, idx_t version,
	                                             optional_ptr<const DeltaMultiFileList> old_snapshot);

private:
	//! Delta tables may be cached in the SchemaEntry. Since the TableEntry holds the snapshot, this allows sharing a
	//! snapshot between different scans.
	unique_ptr<DeltaTableEntry> cached_table;
	mutex lock;
};

} // namespace duckdb
