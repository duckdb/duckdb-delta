// Native C++ acceptance-test harness for the duckdb-delta extension.
//
// Runs the delta-kernel-rs `acceptance/workloads` read specs through DuckDB's
// `delta_scan` IN-PROCESS and compares the result to the golden
// `expected/<spec>/expected_data/*.parquet`, order-insensitively (multiset
// equality via EXCEPT ALL both directions + row-count).
//
// This is the C++ analogue of ~/duckdb/plan-based-scan/workloads_harness.py
// (and its parallel variant). It reproduces the SAME spec selection / skip
// rules so the pass numbers are directly comparable. Unlike the Python driver
// -- which shells out to the `duckdb` CLI one process per shard -- this harness
// runs many DuckDB Connections on ONE shared DuckDB instance across std::threads,
// which is what lets it exercise the async scan state-machine path
// (DELTA_KERNEL_PLAN_SM_ASYNC) under real intra-process concurrency.
//
// The extension reads its mode env vars itself (DELTA_KERNEL_PLAN_SM,
// DELTA_KERNEL_PLAN_SM_ASYNC, DELTA_KERNEL_PLAN_SQL_TVF, DELTA_KERNEL_PLAN_SQL,
// ...). The harness just inherits the environment.

#include "duckdb.hpp"
#include "yyjson.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace duckdb;
using namespace duckdb_yyjson;

namespace {

// ---------------------------------------------------------------------------
// filesystem helpers
// ---------------------------------------------------------------------------
std::string ExpandHome(const std::string &path) {
	if (!path.empty() && path[0] == '~') {
		const char *home = std::getenv("HOME");
		if (home) {
			return std::string(home) + path.substr(1);
		}
	}
	return path;
}

bool PathExists(const std::string &p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

bool IsDir(const std::string &p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// List immediate entries of a directory (names only, no . / ..).
std::vector<std::string> ListDir(const std::string &dir) {
	std::vector<std::string> out;
	DIR *d = opendir(dir.c_str());
	if (!d) {
		return out;
	}
	struct dirent *e;
	while ((e = readdir(d)) != nullptr) {
		std::string name = e->d_name;
		if (name == "." || name == "..") {
			continue;
		}
		out.push_back(name);
	}
	closedir(d);
	return out;
}

std::string ReadFile(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// ---------------------------------------------------------------------------
// SQL string helpers (mirror workloads_harness.py)
// ---------------------------------------------------------------------------

// col_sql(): quote a column reference (leave _metadata.* bare, as in Python).
std::string ColSql(const std::string &c) {
	if (c.rfind("_metadata", 0) == 0) {
		return c;
	}
	std::string escaped;
	for (char ch : c) {
		if (ch == '"') {
			escaped += "\"\"";
		} else {
			escaped += ch;
		}
	}
	return "\"" + escaped + "\"";
}

std::string ReplaceAll(std::string s, const std::string &from, const std::string &to) {
	if (from.empty()) {
		return s;
	}
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
	return s;
}

// Case-insensitive replace of TIMESTAMP_NTZ<optional-space>' with TIMESTAMP '.
std::string ReplaceTimestampNtz(const std::string &in) {
	static const std::string tok = "timestamp_ntz";
	std::string lower = in;
	for (auto &ch : lower) {
		ch = (char)std::tolower((unsigned char)ch);
	}
	std::string out;
	size_t i = 0;
	while (i < in.size()) {
		if (lower.compare(i, tok.size(), tok) == 0) {
			size_t j = i + tok.size();
			while (j < in.size() && (in[j] == ' ' || in[j] == '\t')) {
				j++;
			}
			if (j < in.size() && in[j] == '\'') {
				out += "TIMESTAMP '";
				i = j + 1;
				continue;
			}
		}
		out += in[i];
		i++;
	}
	return out;
}

// translate_predicate(): Spark/DataFusion dialect -> DuckDB dialect.
std::string TranslatePredicate(const std::string &pred) {
	if (pred.empty()) {
		return pred;
	}
	std::string out = ReplaceTimestampNtz(pred);
	out = ReplaceAll(out, "<=>", " IS NOT DISTINCT FROM ");
	out = ReplaceAll(out, "`", "\"");
	return out;
}

// ---------------------------------------------------------------------------
// yyjson accessors
// ---------------------------------------------------------------------------
std::string JsonGetStr(yyjson_val *obj, const char *key) {
	yyjson_val *v = yyjson_obj_get(obj, key);
	if (v && yyjson_is_str(v)) {
		return yyjson_get_str(v);
	}
	return "";
}

// ---------------------------------------------------------------------------
// classification (skip / df_fail) -- mirrors load_classification() in Python
// ---------------------------------------------------------------------------
struct Classification {
	std::vector<std::string> skip;
	std::vector<std::string> kernel_fail;
	std::vector<std::string> fixed_in_df;
	std::vector<std::string> additional_df;
};

std::vector<std::string> JsonStrArray(yyjson_val *root, const char *key) {
	std::vector<std::string> out;
	yyjson_val *arr = yyjson_obj_get(root, key);
	if (!arr || !yyjson_is_arr(arr)) {
		return out;
	}
	yyjson_val *v;
	yyjson_arr_iter it = yyjson_arr_iter_with(arr);
	while ((v = yyjson_arr_iter_next(&it)) != nullptr) {
		if (yyjson_is_str(v)) {
			out.push_back(yyjson_get_str(v));
		}
	}
	return out;
}

bool LoadClassification(const std::string &path, Classification &cls) {
	if (!PathExists(path)) {
		return false;
	}
	std::string txt = ReadFile(path);
	yyjson_doc *doc = yyjson_read(txt.c_str(), txt.size(), 0);
	if (!doc) {
		return false;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	cls.skip = JsonStrArray(root, "skip");
	cls.kernel_fail = JsonStrArray(root, "kernel_fail");
	cls.fixed_in_df = JsonStrArray(root, "fixed_in_df");
	cls.additional_df = JsonStrArray(root, "additional_df");
	yyjson_doc_free(doc);
	return true;
}

bool ContainsAny(const std::string &s, const std::vector<std::string> &pats) {
	for (auto &p : pats) {
		if (s.find(p) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool ShouldSkip(const std::string &spec_path, const Classification &cls) {
	return ContainsAny(spec_path, cls.skip);
}

// df_expected_failure(): fixed-in-df wins, else in kernel_fail|additional_df.
bool DfExpectedFailure(const std::string &spec_path, const Classification &cls) {
	if (ContainsAny(spec_path, cls.fixed_in_df)) {
		return false;
	}
	if (ContainsAny(spec_path, cls.kernel_fail)) {
		return true;
	}
	if (ContainsAny(spec_path, cls.additional_df)) {
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// spec model
// ---------------------------------------------------------------------------
struct Spec {
	std::string path;          // absolute spec json path
	std::string rel;           // path relative to workloads dir (for reporting)
	std::string table;         // <workload>/delta
	std::string expected_glob; // <workload>/expected/<base>/expected_data/*.parquet
	bool has_version = false;
	int64_t version = 0;
	std::string predicate;              // may be empty
	std::vector<std::string> columns;   // empty -> "*"
};

// Build the multiset-equality comparison SQL (mirrors build_comparison_sql()).
std::string BuildComparisonSql(const Spec &s) {
	std::string cols;
	if (s.columns.empty()) {
		cols = "*";
	} else {
		for (size_t i = 0; i < s.columns.size(); i++) {
			if (i) {
				cols += ", ";
			}
			cols += ColSql(s.columns[i]);
		}
	}
	std::string ver;
	if (s.has_version) {
		ver = ", version=" + std::to_string(s.version);
	}
	std::string pred = TranslatePredicate(s.predicate);
	std::string where = pred.empty() ? "" : (" WHERE " + pred);
	std::string actual = "SELECT " + cols + " FROM delta_scan('" + s.table + "'" + ver + ")" + where;
	std::string expected = "SELECT " + cols + " FROM read_parquet('" + s.expected_glob + "')";
	// Single boolean scalar: count match AND both EXCEPT ALL directions empty.
	std::ostringstream q;
	q << "SELECT (\n"
	  << "  (SELECT count(*) FROM (" << actual << ")) = (SELECT count(*) FROM (" << expected << "))\n"
	  << "  AND (SELECT count(*) FROM ((" << actual << ") EXCEPT ALL (" << expected << "))) = 0\n"
	  << "  AND (SELECT count(*) FROM ((" << expected << ") EXCEPT ALL (" << actual << "))) = 0\n"
	  << ");";
	return q.str();
}

// ---------------------------------------------------------------------------
// spec discovery + selection (mirrors the parallel harness runset)
// ---------------------------------------------------------------------------
std::vector<Spec> DiscoverSpecs(const std::string &workloads, const Classification &cls,
                                const std::string &filter, size_t &n_read, size_t &n_skipped) {
	std::vector<Spec> out;
	n_read = 0;
	n_skipped = 0;

	std::vector<std::string> wls = ListDir(workloads);
	std::sort(wls.begin(), wls.end());
	for (auto &wl : wls) {
		std::string wl_dir = workloads + "/" + wl;
		std::string specs_dir = wl_dir + "/specs";
		if (!IsDir(specs_dir)) {
			continue;
		}
		std::vector<std::string> spec_files = ListDir(specs_dir);
		std::sort(spec_files.begin(), spec_files.end());
		for (auto &sf : spec_files) {
			if (sf.size() < 5 || sf.substr(sf.size() - 5) != ".json") {
				continue;
			}
			std::string spec_path = specs_dir + "/" + sf;
			std::string txt = ReadFile(spec_path);
			yyjson_doc *doc = yyjson_read(txt.c_str(), txt.size(), 0);
			if (!doc) {
				continue;
			}
			yyjson_val *root = yyjson_doc_get_root(doc);
			if (!root || !yyjson_is_obj(root)) {
				yyjson_doc_free(doc);
				continue;
			}
			if (JsonGetStr(root, "type") != "read") {
				yyjson_doc_free(doc);
				continue;
			}
			n_read++;

			if (!filter.empty() && spec_path.find(filter) == std::string::npos) {
				yyjson_doc_free(doc);
				continue;
			}

			// --- skip / df_fail classification ---
			if (ShouldSkip(spec_path, cls) || DfExpectedFailure(spec_path, cls)) {
				n_skipped++;
				yyjson_doc_free(doc);
				continue;
			}

			yyjson_val *expected = yyjson_obj_get(root, "expected");
			// expected:null -> negative/error spec, not a data-parity target.
			if (!expected || yyjson_is_null(expected)) {
				n_skipped++;
				yyjson_doc_free(doc);
				continue;
			}
			// error block w/o rowCount -> negative/error spec.
			if (yyjson_is_obj(expected)) {
				bool has_error = yyjson_obj_get(expected, "error") != nullptr;
				bool has_rowcount = yyjson_obj_get(expected, "rowCount") != nullptr;
				if (has_error && !has_rowcount) {
					n_skipped++;
					yyjson_doc_free(doc);
					continue;
				}
			}

			yyjson_val *ts = yyjson_obj_get(root, "timestamp");
			yyjson_val *ver = yyjson_obj_get(root, "version");
			bool has_version = ver && yyjson_is_int(ver);
			// timestamp-based time travel without a version -> unsupported via delta_scan; skip.
			if (ts && !yyjson_is_null(ts) && !has_version) {
				n_skipped++;
				yyjson_doc_free(doc);
				continue;
			}

			Spec s;
			s.path = spec_path;
			s.rel = wl + "/specs/" + sf;
			s.table = wl_dir + "/delta";
			std::string base = sf.substr(0, sf.size() - 5);
			s.expected_glob = wl_dir + "/expected/" + base + "/expected_data/*.parquet";
			s.has_version = has_version;
			if (has_version) {
				s.version = yyjson_get_sint(ver);
			}
			s.predicate = JsonGetStr(root, "predicate");
			yyjson_val *columns = yyjson_obj_get(root, "columns");
			if (columns && yyjson_is_arr(columns)) {
				yyjson_val *cv;
				yyjson_arr_iter it = yyjson_arr_iter_with(columns);
				while ((cv = yyjson_arr_iter_next(&it)) != nullptr) {
					if (yyjson_is_str(cv)) {
						s.columns.push_back(yyjson_get_str(cv));
					}
				}
			}
			out.push_back(std::move(s));
			yyjson_doc_free(doc);
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// run one spec on a given connection: returns "PASS" / "FAIL" / "ERROR"
// ---------------------------------------------------------------------------
enum class Outcome { PASS, FAIL, ERROR };

Outcome RunSpec(Connection &con, const Spec &s, std::string &err_msg) {
	std::string sql = BuildComparisonSql(s);
	unique_ptr<MaterializedQueryResult> r;
	try {
		r = con.Query(sql);
	} catch (const std::exception &ex) {
		err_msg = ex.what();
		return Outcome::ERROR;
	}
	if (!r || r->HasError()) {
		err_msg = r ? r->GetError() : "null result";
		return Outcome::ERROR;
	}
	if (r->RowCount() == 0 || r->ColumnCount() == 0) {
		err_msg = "no result row";
		return Outcome::ERROR;
	}
	Value v = r->GetValue(0, 0);
	if (v.IsNull()) {
		// NULL comparison result (e.g. one side errored inside the scalar) -> treat as fail.
		return Outcome::FAIL;
	}
	bool ok = v.GetValue<bool>();
	return ok ? Outcome::PASS : Outcome::FAIL;
}

} // namespace

int main(int argc, char **argv) {
	std::string workloads = ExpandHome("~/delta-kernel-rs/acceptance/workloads");
	std::string classification_path = ExpandHome("~/duckdb/duckdb-delta/test/acceptance_classification.json");
	std::string extension_path = ExpandHome("~/duckdb/duckdb-delta/build/release/extension/delta/delta.duckdb_extension");
	std::string filter;
	int concurrency = 0; // 0 = sequential
	bool verbose_pass = false;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&](const char *name) -> std::string {
			if (i + 1 >= argc) {
				std::cerr << "missing value for " << name << "\n";
				std::exit(2);
			}
			return argv[++i];
		};
		if (a == "--workloads") {
			workloads = ExpandHome(next("--workloads"));
		} else if (a == "--classification") {
			classification_path = ExpandHome(next("--classification"));
		} else if (a == "--extension") {
			extension_path = ExpandHome(next("--extension"));
		} else if (a == "--concurrency") {
			concurrency = std::stoi(next("--concurrency"));
		} else if (a == "--filter") {
			filter = next("--filter");
		} else if (a == "--verbose") {
			verbose_pass = true;
		} else if (a == "--help" || a == "-h") {
			std::cout << "Usage: acceptance_harness [--workloads DIR] [--classification JSON] "
			             "[--extension PATH] [--concurrency N] [--filter SUBSTR] [--verbose]\n";
			return 0;
		} else {
			std::cerr << "unknown arg: " << a << "\n";
			return 2;
		}
	}

	Classification cls;
	if (!LoadClassification(classification_path, cls)) {
		std::cerr << "[warn] could not load classification " << classification_path
		          << " -- proceeding with NO skip/df-fail patterns (numbers won't match Python)\n";
	}
	std::cerr << "[classification] skip=" << cls.skip.size() << " kernel_fail=" << cls.kernel_fail.size()
	          << " fixed_in_df=" << cls.fixed_in_df.size() << " additional_df=" << cls.additional_df.size() << "\n";

	size_t n_read = 0, n_skipped = 0;
	std::vector<Spec> specs = DiscoverSpecs(workloads, cls, filter, n_read, n_skipped);
	std::cerr << "[plan] read_specs=" << n_read << " skipped=" << n_skipped
	          << " target(runset)=" << specs.size() << "\n";
	if (specs.empty()) {
		std::cerr << "no specs to run\n";
		return 1;
	}

	// One shared DuckDB instance; unsigned extensions allowed so we can LOAD by path.
	DBConfig config;
	config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
	DuckDB db(nullptr, &config);
	{
		Connection con(db);
		auto r = con.Query("LOAD '" + extension_path + "';");
		if (!r || r->HasError()) {
			std::cerr << "[fatal] failed to LOAD extension '" << extension_path
			          << "': " << (r ? r->GetError() : "null") << "\n";
			return 1;
		}
	}

	std::atomic<size_t> n_pass {0}, n_fail {0}, n_error {0};
	std::mutex print_mtx;

	auto report = [&](const Spec &s, Outcome o, const std::string &msg) {
		if (o == Outcome::PASS) {
			n_pass++;
			if (verbose_pass) {
				std::lock_guard<std::mutex> lk(print_mtx);
				std::cout << "PASS  " << s.rel << "\n";
			}
			return;
		}
		std::lock_guard<std::mutex> lk(print_mtx);
		if (o == Outcome::FAIL) {
			n_fail++;
			std::cout << "FAIL  " << s.rel << "\n";
		} else {
			n_error++;
			std::string first_line = msg.substr(0, msg.find('\n'));
			std::cout << "ERROR " << s.rel << "  :: " << first_line << "\n";
		}
	};

	auto run_range = [&](size_t begin, size_t stride) {
		Connection con(db);
		for (size_t i = begin; i < specs.size(); i += stride) {
			std::string msg;
			Outcome o = RunSpec(con, specs[i], msg);
			report(specs[i], o, msg);
		}
	};

	if (concurrency <= 0) {
		run_range(0, 1);
	} else {
		std::vector<std::thread> threads;
		size_t nthreads = (size_t)concurrency;
		for (size_t t = 0; t < nthreads; t++) {
			threads.emplace_back(run_range, t, nthreads);
		}
		for (auto &th : threads) {
			th.join();
		}
	}

	size_t total = specs.size();
	std::cout << "\n===== SUMMARY =====\n";
	std::cout << "mode: " << (concurrency <= 0 ? "sequential" : ("concurrency=" + std::to_string(concurrency))) << "\n";
	std::cout << "pass=" << n_pass.load() << "/" << total << "  fail=" << n_fail.load()
	          << "  error=" << n_error.load() << "\n";
	return (n_fail.load() == 0 && n_error.load() == 0) ? 0 : 1;
}
