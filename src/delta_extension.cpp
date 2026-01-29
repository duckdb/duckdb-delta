#include "delta_extension.hpp"

#include "delta_utils.hpp"
#include "delta_functions.hpp"
#include "delta_log_types.hpp"
#include "delta_macros.hpp"
#include "storage/delta_catalog.hpp"
#include "storage/delta_transaction_manager.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

static unique_ptr<Catalog> DeltaCatalogAttach(optional_ptr<StorageExtensionInfo> storage_info,
                                                     ClientContext &context, AttachedDatabase &db, const string &name,
                                                     AttachInfo &info, AttachOptions &options) {

	auto res = make_uniq<DeltaCatalog>(db, info.path, options.access_mode);
    res->internal_table_name = name;

	for (const auto &option : info.options) {
		if (StringUtil::Lower(option.first) == "pin_snapshot") {
			res->use_cache = option.second.GetValue<bool>();
		}
		if (StringUtil::Lower(option.first) == "pushdown_partition_info") {
			res->pushdown_partition_info = option.second.GetValue<bool>();
		}
		if (StringUtil::Lower(option.first) == "pushdown_filters") {
			auto str = option.second.GetValue<string>();
			res->filter_pushdown_mode = DeltaEnumUtils::FromString(str);
		}
	    if (StringUtil::Lower(option.first) == "version") {
	        res->use_cache = true;
	        res->use_specific_version = UBigIntValue::Get(option.second.DefaultCastAs(LogicalType::UBIGINT));
	    }
	    if (StringUtil::Lower(option.first) == "internal_table_name") {
	        res->internal_table_name = StringValue::Get(option.second);
	    }
	    if (StringUtil::Lower(option.first) == "child_catalog_mode") {
	        res->child_catalog_mode = option.second.GetValue<bool>();
	    }
	}

	res->SetDefaultTable(DEFAULT_SCHEMA, res->GetInternalTableName());

	return std::move(res);
}

static unique_ptr<TransactionManager> CreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                        AttachedDatabase &db, Catalog &catalog) {
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();
	return make_uniq<DeltaTransactionManager>(db, delta_catalog);
}

class DeltaStorageExtension : public StorageExtension {
public:
	DeltaStorageExtension() {
		attach = DeltaCatalogAttach;
		create_transaction_manager = CreateTransactionManager;
	}
};

static void LoadInternal(ExtensionLoader &loader) {
	// Load Table functions
    for (const auto &function : DeltaFunctions::GetTableFunctions(loader)) {
        loader.RegisterFunction(function);
	}

	// Load Scalar functions
    for (const auto &function : DeltaFunctions::GetScalarFunctions(loader)) {
        loader.RegisterFunction(function);
	}

	// Register the "single table" delta catalog (to ATTACH a single delta table)
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	StorageExtension::Register(config, "delta", make_shared_ptr<DeltaStorageExtension>());

	config.AddExtensionOption("delta_scan_explain_files_filtered",
	                          "Adds the filtered files to the explain output. Warning: this may impact performance of "
	                          "delta scan during explain analyze queries.",
	                          LogicalType::BOOLEAN, Value(true));

	config.AddExtensionOption(
	    "delta_kernel_logging",
	    "Forwards the internal logging of the Delta Kernel to the duckdb logger. Warning: this may impact "
	    "performance even with DuckDB logging disabled.",
	    LogicalType::BOOLEAN, Value(false), LoggerCallback::DuckDBSettingCallBack);

	DeltaMacros::RegisterMacros(loader);

	DeltaLogTypes::RegisterLogTypes(loader.GetDatabaseInstance());

	LoggerCallback::Initialize(loader.GetDatabaseInstance());
}

void DeltaExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DeltaExtension::Name() {
	return "delta";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(delta, loader) {
    duckdb::LoadInternal(loader);
}

}
