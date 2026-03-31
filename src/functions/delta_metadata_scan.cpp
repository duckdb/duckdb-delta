#include "delta_functions.hpp"
#include "functions/delta_scan/delta_multi_file_reader.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

struct DeltaFileListOptions {
	bool transform_expression = false;
	bool delete_count = false;
};

static void AddFileInfo(OpenFileInfo &file_info, DeltaFileMetaData &metadata, vector<Value> &row_values,
                        DeltaFileListOptions &options) {
	//! PATH
	row_values.emplace_back(file_info.path);

	//! CARDINALITY
	row_values.push_back(Value::UBIGINT(metadata.cardinality));

	//! PARTITIONS
	auto partitions = metadata.partition_map;
	InsertionOrderPreservingMap<string> map;
	for (auto &kv : partitions) {
		map[kv.first] = kv.second.ToString();
	}
	row_values.emplace_back(Value::MAP(map));

	//! HAVE_DELETES
	row_values.emplace_back(Value(metadata.selection_vector.ptr != nullptr));

	//! DELETE_COUNT
	if (options.delete_count) {
		idx_t delete_count = 0;
		if (metadata.selection_vector.ptr != nullptr) {
			for (idx_t i = 0; i < metadata.selection_vector.len; i++) {
				if (!metadata.selection_vector.ptr[i]) {
					delete_count++;
				}
			}
		}
		row_values.emplace_back(Value::UBIGINT(delete_count));
	}

	//! TRANSFORM EXPRESSION
	if (options.transform_expression) {
		if (metadata.transform_expression) {
			vector<Value> transform_expression_value;
			for (const auto &expr : *metadata.transform_expression) {
				if (expr) {
					transform_expression_value.emplace_back(expr->ToString());
				}
			}
			row_values.emplace_back(Value::LIST(transform_expression_value));
		} else {
			row_values.emplace_back(Value());
		}
	}
}

static unique_ptr<FunctionData> DeltaFileListBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	//! Parse input

	auto input_string = input.inputs[0].ToString();
	idx_t version = DConstants::INVALID_INDEX;
	DeltaFileListOptions options;

	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		auto &val = kv.second;
		if (loption == "version") {
			version = val.GetValue<idx_t>();
		} else if (loption == "transform_expression") {
			options.transform_expression = val.GetValue<bool>();
		} else if (loption == "delete_count") {
			options.delete_count = val.GetValue<bool>();
		}
	}

	//! Create multifilelist

	auto file_list = make_uniq<DeltaMultiFileList>(context, input_string, version);

	// TODO: this is weird
	vector<string> _n;
	vector<LogicalType> _t;
	file_list->Bind(_t, _n);

	//! Define function schema

	names.emplace_back("data_file");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("cardinality");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("partitions");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("have_deletes");
	return_types.emplace_back(LogicalType::BOOLEAN);

	if (options.delete_count) {
		names.emplace_back("delete_count");
		return_types.emplace_back(LogicalType::UBIGINT);
	}

	if (options.transform_expression) {
		names.emplace_back("transform_expression");
		return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	}

	auto files = file_list->GetAllFiles();

	// generate the result
	auto result = make_uniq<MetadataBindData>();
	for (idx_t i = 0; i < files.size(); ++i) {
		auto &file = files[i];
		auto &metadata = file_list->GetMetaData(i);

		vector<Value> row_values;
		AddFileInfo(file, metadata, row_values, options);
		result->rows.push_back(row_values);
	}

	return std::move(result);
}

DeltaFileListFunction::DeltaFileListFunction() : DeltaBaseMetadataFunction("delta_metadata", DeltaFileListBind) {
	// arguments.push_back(LogicalType::VARCHAR);
	named_parameters.insert({"transform_expression", LogicalType::BOOLEAN});
	named_parameters.insert({"delete_count", LogicalType::BOOLEAN});
	named_parameters.insert({"version", LogicalType::UBIGINT});
}

TableFunctionSet DeltaFunctions::GetDeltaFileListFunction(ExtensionLoader &loader) {
	TableFunctionSet function_set("delta_list_files");

	DeltaFileListFunction fun;
	function_set.AddFunction(fun);

	return function_set;
}

} // namespace duckdb
