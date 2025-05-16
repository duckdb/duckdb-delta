#include "delta_macros.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"

namespace duckdb {

// TODO: use Structured logging for this
// Macro to fetch the pushed down filters for the most recent query
static constexpr auto DELTA_FILTER_PUSHDOWN_MACRO = R"(
SELECT
    l1.message as query_string,
    parse_delta_filter_logline(l2.message)['type'] as filter_type,
    parse_delta_filter_logline(l2.message)['files_before'] as files_before,
    parse_delta_filter_logline(l2.message)['files_after'] as files_after,
    parse_delta_filter_logline(l2.message)['filters_before'] as filters_before,
    parse_delta_filter_logline(l2.message)['filters_after'] as filters_after,
    parse_delta_filter_logline(l2.message)['path'] as path
FROM duckdb_logs as l1
JOIN duckdb_logs as l2 ON
    l1.transaction_id = l2.transaction_id
WHERE
    l2.type='delta.FilterPushdown' AND
    l1.type = 'QueryLog'
ORDER BY l1.transaction_id
)";

static constexpr auto DELTA_FILTER_PUSHDOWN_MACRO_TPCDS = R"(
SELECT
    query_nr as tpcds_query,
    filter_type,
    files_before,
    files_after,
    filters_before,
    filters_after,
    path
FROM
    delta_filter_pushdown_log()
JOIN
    tpcds_queries() as tpcds_queries on tpcds_queries."query"=query_string
)";

void DeltaMacros::RegisterTableMacro(DatabaseInstance &db, const string &name, const string &query,
                                     const vector<string> &params, const child_list_t<Value> &named_params) {
	Parser parser;
	parser.ParseQuery(query);
	const auto &stmt = parser.statements.back();
	auto &node = stmt->Cast<SelectStatement>().node;

	auto func = make_uniq<TableMacroFunction>(std::move(node));
	for (auto &param : params) {
		func->parameters.push_back(make_uniq<ColumnRefExpression>(param));
	}

	for (auto &param : named_params) {
		func->default_parameters[param.first] = make_uniq<ConstantExpression>(param.second);
	}

	CreateMacroInfo info(CatalogType::TABLE_MACRO_ENTRY);
	info.schema = DEFAULT_SCHEMA;
	info.name = name;
	info.temporary = true;
	info.internal = true;
	info.macros.push_back(std::move(func));

	ExtensionUtil::RegisterFunction(db, info);
}

static DefaultMacro delta_macros[] = {
    {DEFAULT_SCHEMA,
     "parse_delta_filter_logline",
     {"x", nullptr},
     {{nullptr, nullptr}},
     "x::STRUCT(path VARCHAR, type VARCHAR, filters_before VARCHAR[], filters_after VARCHAR[], files_before BIGINT, "
     "files_after BIGINT)"},
};

void DeltaMacros::RegisterMacros(DatabaseInstance &instance) {
	// Register Regular macros
	for (auto &macro : delta_macros) {
		auto info = DefaultFunctionGenerator::CreateInternalMacroInfo(macro);
		ExtensionUtil::RegisterFunction(instance, *info);
	}

	// Register Table Macros
	RegisterTableMacro(instance, "delta_filter_pushdown_log", DELTA_FILTER_PUSHDOWN_MACRO, {}, {});
	RegisterTableMacro(instance, "delta_filter_pushdown_log_tpcds", DELTA_FILTER_PUSHDOWN_MACRO_TPCDS, {}, {});
}

}; // namespace duckdb
