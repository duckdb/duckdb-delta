#include "functions/delta_copy.hpp"

#include "storage/delta_create_table.hpp"
#include "storage/delta_insert.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

namespace {

//! The bound COPY: the query below it, and the table to create from it
class DeltaLogicalCopy : public LogicalExtensionOperator {
public:
	explicit DeltaLogicalCopy(DeltaCreateTableRequest request_p) : request(std::move(request_p)) {
	}

	DeltaCreateTableRequest request;

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override {
		D_ASSERT(children.size() == 1);
		auto &child = planner.CreatePlan(*children[0]);
		return DeltaInsert::PlanCreateTableAs(context, planner, child, std::move(request), "Delta COPY TO",
		                                      estimated_cardinality);
	}

protected:
	void ResolveTypes() override {
		types = {LogicalType::BIGINT};
	}
};

//! COPY lowercases option names and does not parse a dotted one, while Delta property keys are dotted and
//! case-sensitive (`delta.columnMapping.mode`), so the table properties come as one option: a MAP or a STRUCT
static vector<pair<string, string>> GetTableProperties(const vector<Value> &values) {
	if (values.size() != 1 || values[0].IsNull()) {
		throw BinderException("Delta COPY TO: TABLE_PROPERTIES takes one MAP or STRUCT, e.g. "
		                      "TABLE_PROPERTIES {'delta.appendOnly': 'true'}");
	}
	auto &value = values[0];
	vector<pair<string, string>> result;
	auto add = [&](const string &key, const Value &property) {
		if (property.IsNull()) {
			throw BinderException("NULL is not a valid value for Delta table property '%s'", key);
		}
		result.emplace_back(key, property.ToString());
	};
	switch (value.type().id()) {
	case LogicalTypeId::STRUCT: {
		auto &children = StructValue::GetChildren(value);
		for (idx_t i = 0; i < children.size(); i++) {
			add(StructType::GetChildName(value.type(), i).GetIdentifierName(), children[i]);
		}
		break;
	}
	case LogicalTypeId::MAP:
		for (auto &entry : MapValue::GetChildren(value)) {
			auto &key_value = StructValue::GetChildren(entry);
			add(key_value[0].ToString(), key_value[1]);
		}
		break;
	default:
		throw BinderException("Delta COPY TO: TABLE_PROPERTIES takes a MAP or a STRUCT, not %s",
		                      value.type().ToString());
	}
	std::sort(result.begin(), result.end());
	return result;
}

static void DeltaCopyOptions(ClientContext &context, CopyOptionsInput &input) {
	input.options["partition_by"] = CopyOption(LogicalType::LIST(LogicalType::VARCHAR), CopyOptionMode::WRITE_ONLY);
	input.options["table_properties"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
}

vector<string> GetPartitionColumns(const vector<Value> &values, const vector<Identifier> &names) {
	vector<string> result;
	for (auto &value : values) {
		if (value.IsNull()) {
			throw BinderException("Delta COPY TO: PARTITION_BY takes column names, not NULL");
		}
		auto name = value.ToString();
		bool found = false;
		for (auto &column : names) {
			found |= column.GetIdentifierName() == name;
		}
		if (!found) {
			throw BinderException("Delta COPY TO: PARTITION_BY names column '%s', which the query does not return",
			                      name);
		}
		result.push_back(std::move(name));
	}
	return result;
}

BoundStatement DeltaCopyPlan(Binder &binder, CopyStatement &stmt) {
	auto &info = *stmt.info;
	if (info.file_path.empty()) {
		throw BinderException("Delta COPY TO needs the path of the table to create");
	}

	auto node = info.select_statement->Copy();
	auto query = binder.Bind(*node);

	DeltaCreateTableRequest request;
	request.path = info.file_path;
	request.data_change = true;
	for (idx_t i = 0; i < query.names.size(); i++) {
		request.columns.AddColumn(ColumnDefinition(query.names[i], query.types[i]));
	}

	for (auto &option : info.options) {
		auto key = option.first.GetIdentifierName();
		if (StringUtil::CIEquals(key, "partition_by")) {
			request.partition_columns = GetPartitionColumns(option.second, query.names);
			continue;
		}
		if (StringUtil::CIEquals(key, "table_properties")) {
			request.properties = GetTableProperties(option.second);
			continue;
		}
		// COPY's generic options (OVERWRITE, FILENAME_PATTERN, ...) pass its own check, but say nothing about a table
		throw NotImplementedException("Delta COPY TO does not take the option '%s': it takes PARTITION_BY and "
		                              "TABLE_PROPERTIES. It always creates a new table, and writes the files as "
		                              "the table needs them",
		                              key);
	}

	auto copy = make_uniq<DeltaLogicalCopy>(std::move(request));
	copy->children.push_back(std::move(query.plan));

	BoundStatement result;
	result.types = {LogicalType::BIGINT};
	result.names = {"Count"};
	result.plan = std::move(copy);
	return result;
}

} // namespace

CopyFunction DeltaCopyFunction::Create() {
	CopyFunction function("delta");
	function.plan = DeltaCopyPlan;
	function.copy_options = DeltaCopyOptions;
	return function;
}

} // namespace duckdb
