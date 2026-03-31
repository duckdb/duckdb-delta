#include "delta_functions.hpp"
#include "delta_utils.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

namespace duckdb {

struct DomainMetadataVisitorData {
	vector<vector<Value>> rows;
};

static void DomainMetadataVisitor(ffi::NullableCvoid engine_context, ffi::KernelStringSlice domain,
                                  ffi::KernelStringSlice configuration) {
	auto &data = *static_cast<DomainMetadataVisitorData *>(const_cast<void *>(engine_context));
	vector<Value> row;
	row.emplace_back(KernelUtils::FromDeltaString(domain));
	row.emplace_back(KernelUtils::FromDeltaString(configuration));
	data.rows.push_back(std::move(row));
}

static unique_ptr<FunctionData> DeltaDomainMetadataBind(ClientContext &context, TableFunctionBindInput &input,
                                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto input_string = input.inputs[0].ToString();
	idx_t version = DConstants::INVALID_INDEX;

	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "version") {
			version = kv.second.GetValue<idx_t>();
		}
	}

	auto file_list = make_uniq<DeltaMultiFileList>(context, input_string, version);

	// Trigger snapshot initialization
	vector<string> _n;
	vector<LogicalType> _t;
	file_list->Bind(_t, _n);

	// Define output schema
	names.emplace_back("domain");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("configuration");
	return_types.emplace_back(LogicalType::VARCHAR);

	// Visit domain metadata from the snapshot
	DomainMetadataVisitorData visitor_data;
	{
		auto snapshot_ref = file_list->snapshot->GetLockingRef();
		bool has_metadata;
		auto error = KernelUtils::TryUnpackResult(ffi::visit_domain_metadata(snapshot_ref.GetPtr(),
		                                                                     file_list->extern_engine.get(),
		                                                                     &visitor_data, DomainMetadataVisitor),
		                                          has_metadata);
		if (error.HasError()) {
			error.Throw();
		}
	}

	auto result = make_uniq<MetadataBindData>();
	result->rows = std::move(visitor_data.rows);
	return std::move(result);
}

DeltaDomainMetadataFunction::DeltaDomainMetadataFunction()
    : DeltaBaseMetadataFunction("delta_domain_metadata", DeltaDomainMetadataBind) {
	named_parameters.insert({"version", LogicalType::UBIGINT});
}

TableFunctionSet DeltaFunctions::GetDeltaDomainMetadataFunction(ExtensionLoader &loader) {
	TableFunctionSet function_set("delta_domain_metadata");

	DeltaDomainMetadataFunction fun;
	function_set.AddFunction(fun);

	return function_set;
}

} // namespace duckdb
