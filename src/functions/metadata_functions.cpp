#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

#include "delta_utils.hpp"
#include "delta_functions.hpp"

namespace duckdb {

struct MetadataFunctionData : public GlobalTableFunctionState {
	MetadataFunctionData() : offset(0) {
	}

	idx_t offset;
};

static unique_ptr<GlobalTableFunctionState> MetadataFunctionInit(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto result = make_uniq<MetadataFunctionData>();
	return std::move(result);
}

static void MetadataFunctionExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<MetadataBindData>();
	auto &state = data_p.global_state->Cast<MetadataFunctionData>();
	if (state.offset >= data.rows.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.rows[state.offset++];
		if (entry.size() != output.ColumnCount()) {
			throw InternalException("Unaligned metadata row in result");
		}

		for (idx_t c = 0; c < entry.size(); c++) {
			output.SetValue(c, count, entry[c]);
		}
		count++;
	}
	output.SetCardinality(count);
}

DeltaBaseMetadataFunction::DeltaBaseMetadataFunction(string name_p, table_function_bind_t bind)
    : TableFunction(std::move(name_p), {LogicalType::VARCHAR}, MetadataFunctionExecute, bind, MetadataFunctionInit) {
}

} // namespace duckdb
