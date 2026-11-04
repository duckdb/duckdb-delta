#include "functions/delta_copy.hpp"

#include "delta_table_properties.hpp"
#include "storage/delta_create_table.hpp"
#include "storage/delta_insert.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/query_result.hpp"
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
//! case-sensitive (`delta.columnMapping.mode`), so the table properties come as one option, a MAP or a STRUCT, keys
//! as given: `WITH_PROPERTIES {...}`, after CREATE's `WITH (...)`
static vector<pair<string, string>> GetWithProperties(const vector<Value> &values) {
	if (values.size() != 1 || values[0].IsNull()) {
		throw BinderException("Delta COPY TO: WITH_PROPERTIES takes one MAP or STRUCT, e.g. "
		                      "WITH_PROPERTIES {'delta.appendOnly': 'true'}");
	}
	auto &value = values[0];
	vector<pair<string, string>> result;
	auto add = [&](const string &key, const Value &property) {
		if (property.IsNull()) {
			throw BinderException("NULL is not a valid value for Delta table property '%s'", key);
		}
		DeltaTableProperties::ThrowIfUnknownDeltaKey(key, "Delta COPY TO");
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
		throw BinderException("Delta COPY TO: WITH_PROPERTIES takes a MAP or a STRUCT, not %s",
		                      value.type().ToString());
	}
	return result;
}

//! Besides PARTITION_BY and WITH_PROPERTIES, every known `delta.*` property is a COPY option of its own, as
//! duckdb-iceberg takes its write properties: `"delta.columnMapping.mode" 'name'`. The option name arrives lowercased
//! and is set as the property's canonical spelling. Registered under that spelling, so that DuckDB's refusal of an
//! unknown option suggests the right key.
static void DeltaCopyOptions(ClientContext &context, CopyOptionsInput &input) {
	input.options["partition_by"] = CopyOption(LogicalType::LIST(LogicalType::VARCHAR), CopyOptionMode::WRITE_ONLY);
	input.options["with_properties"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
	for (auto &key : DeltaTableProperties::KnownKeys()) {
		input.options[Identifier(key)] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
	}
}

vector<string> GetPartitionKeys(const vector<Value> &values) {
	vector<string> result;
	for (auto &value : values) {
		if (value.IsNull()) {
			throw BinderException("Delta COPY TO: PARTITION_BY takes column names, not NULL");
		}
		result.push_back(value.ToString());
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

	// Duplicate names get a suffix, as COPY to a file and CREATE TABLE AS give them
	auto names = query.names;
	QueryResult::DeduplicateColumns(names);

	DeltaCreateTableRequest request;
	request.path = info.file_path;
	request.data_change = true;
	for (idx_t i = 0; i < names.size(); i++) {
		request.columns.AddColumn(ColumnDefinition(names[i], query.types[i]));
	}

	vector<pair<string, string>> bare_properties;
	for (auto &option : info.options) {
		auto key = option.first.GetIdentifierName();
		if (StringUtil::CIEquals(key, "partition_by")) {
			request.partition_columns = ResolvePartitionColumns(request.columns, GetPartitionKeys(option.second),
			                                                    "Delta COPY TO", "PARTITION_BY");
			continue;
		}
		if (StringUtil::CIEquals(key, "with_properties")) {
			auto properties = GetWithProperties(option.second);
			request.properties.insert(request.properties.end(), properties.begin(), properties.end());
			continue;
		}
		auto known = DeltaTableProperties::FindKnownKey(key);
		if (known) {
			if (option.second.size() != 1 || option.second[0].IsNull()) {
				throw BinderException("Delta COPY TO: table property '%s' takes one value", *known);
			}
			bare_properties.emplace_back(*known, option.second[0].ToString());
			continue;
		}
		// COPY's generic options (OVERWRITE, FILENAME_PATTERN, ...) pass its own check, but say nothing about a table.
		// Any other unknown option DuckDB refuses before this runs.
		throw NotImplementedException("Delta COPY TO does not take the option '%s': it takes PARTITION_BY, "
		                              "WITH_PROPERTIES {...} and known delta.* table properties. It always creates a "
		                              "new table, and writes the files as the table needs them",
		                              key);
	}
	for (auto &property : bare_properties) {
		for (auto &given : request.properties) {
			if (given.first == property.first) {
				throw BinderException("Delta COPY TO: table property '%s' is given both as an option and in "
				                      "WITH_PROPERTIES; give it once",
				                      property.first);
			}
		}
		request.properties.push_back(property);
	}
	// As CREATE takes its `location` option: it names the path, which here is COPY's target
	TakeLocationProperty(request.properties, request.path, "its target path", "Delta COPY TO");
	std::sort(request.properties.begin(), request.properties.end());
	for (idx_t i = 1; i < request.properties.size(); i++) {
		if (request.properties[i].first == request.properties[i - 1].first) {
			throw BinderException("Delta COPY TO: table property '%s' is given twice", request.properties[i].first);
		}
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
