#include "functions/delta_scan/delta_scan.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "functions/delta_scan/delta_multi_file_reader.hpp"

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include <regex>

namespace duckdb {

static string url_decode(string input) {
	string result;
	result.reserve(input.size());
	char ch;
	for (idx_t i = 0; i < input.length(); i++) {
		if (int(input[i]) == 37) {
			unsigned int ii;
			sscanf(input.substr(i + 1, 2).c_str(), "%x", &ii);
			ch = static_cast<char>(ii);
			result += ch;
			i += 2;
		} else {
			result += input[i];
		}
	}
	return result;
}

static string ParseAccountNameFromEndpoint(const string &endpoint) {
	if (!StringUtil::StartsWith(endpoint, "https://")) {
		return "";
	}
	auto result = endpoint.find('.', 8);
	if (result == endpoint.npos) {
		return "";
	}
	return endpoint.substr(8, result - 8);
}

static string parseFromConnectionString(const string &connectionString, const string &key) {
	std::regex pattern(key + "=([^;]+)(?=;|$)");
	std::smatch matches;
	if (std::regex_search(connectionString, matches, pattern) && matches.size() > 1) {
		// The second match ([1]) contains the access key
		return matches[1].str();
	}
	return "";
}

static ffi::EngineBuilder *CreateBuilder(ClientContext &context, const string &path) {
	ffi::EngineBuilder *builder;

	// For "regular" paths we early out with the default builder config
	if (!StringUtil::StartsWith(path, "s3://") && !StringUtil::StartsWith(path, "gcs://") &&
	    !StringUtil::StartsWith(path, "gs://") && !StringUtil::StartsWith(path, "r2://") &&
	    !StringUtil::StartsWith(path, "azure://") && !StringUtil::StartsWith(path, "az://") &&
	    !StringUtil::StartsWith(path, "abfs://") && !StringUtil::StartsWith(path, "abfss://")) {
		auto interface_builder_res =
		    ffi::get_engine_builder(KernelUtils::ToDeltaString(path), DuckDBEngineError::AllocateError);

		ffi::EngineBuilder *return_value;
		auto res = KernelUtils::TryUnpackResult(interface_builder_res, return_value);
		if (res.HasError()) {
			res.Throw();
		}
		return return_value;
	}

	string bucket;
	string path_in_bucket;
	string secret_type;

	if (StringUtil::StartsWith(path, "s3://")) {
		auto end_of_container = path.find('/', 5);

		if (end_of_container == string::npos) {
			throw IOException("Invalid s3 url passed to delta scan: %s", path);
		}
		bucket = path.substr(5, end_of_container - 5);
		path_in_bucket = path.substr(end_of_container);
		secret_type = "s3";
	} else if (StringUtil::StartsWith(path, "gcs://")) {
		auto end_of_container = path.find('/', 6);

		if (end_of_container == string::npos) {
			throw IOException("Invalid gcs url passed to delta scan: %s", path);
		}
		bucket = path.substr(6, end_of_container - 6);
		path_in_bucket = path.substr(end_of_container);
		secret_type = "gcs";
	} else if (StringUtil::StartsWith(path, "gs://")) {
		auto end_of_container = path.find('/', 5);

		if (end_of_container == string::npos) {
			throw IOException("Invalid gcs url passed to delta scan: %s", path);
		}
		bucket = path.substr(5, end_of_container - 5);
		path_in_bucket = path.substr(end_of_container);
		secret_type = "gcs";
	} else if (StringUtil::StartsWith(path, "r2://")) {
		auto end_of_container = path.find('/', 5);

		if (end_of_container == string::npos) {
			throw IOException("Invalid gcs url passed to delta scan: %s", path);
		}
		bucket = path.substr(5, end_of_container - 5);
		path_in_bucket = path.substr(end_of_container);
		secret_type = "r2";
	} else if ((StringUtil::StartsWith(path, "azure://")) || (StringUtil::StartsWith(path, "abfss://"))) {
		auto end_of_container = path.find('/', 8);

		if (end_of_container == string::npos) {
			throw IOException("Invalid azure url passed to delta scan: %s", path);
		}
		bucket = path.substr(8, end_of_container - 8);
		path_in_bucket = path.substr(end_of_container);
		secret_type = "azure";
	} else if (StringUtil::StartsWith(path, "az://")) {
		auto end_of_container = path.find('/', 5);

		if (end_of_container == string::npos) {
			throw IOException("Invalid azure url passed to delta scan: %s", path);
		}
		bucket = path.substr(5, end_of_container - 5);
		path_in_bucket = path.substr(end_of_container);
		secret_type = "azure";
	} else if (StringUtil::StartsWith(path, "abfs://")) {
		auto end_of_container = path.find('/', 7);

		if (end_of_container == string::npos) {
			throw IOException("Invalid azure url passed to delta scan: %s", path);
		}
		bucket = path.substr(8, end_of_container - 8);
		path_in_bucket = path.substr(end_of_container);
		secret_type = "azure";
	}

	// We need to substitute DuckDB's usage of s3 and r2 paths because delta kernel needs to just interpret them as s3
	// protocol servers.
	string cleaned_path;
	if (StringUtil::StartsWith(path, "r2://") || StringUtil::StartsWith(path, "gs://")) {
		cleaned_path = "s3://" + path.substr(5);
	} else if (StringUtil::StartsWith(path, "gcs://")) {
		cleaned_path = "s3://" + path.substr(6);
	} else {
		cleaned_path = path;
	}

	auto interface_builder_res =
	    ffi::get_engine_builder(KernelUtils::ToDeltaString(cleaned_path), DuckDBEngineError::AllocateError);

	auto res = KernelUtils::TryUnpackResult(interface_builder_res, builder);
	if (res.HasError()) {
		res.Throw();
	}

	// For S3 or Azure paths we need to trim the url, set the container, and fetch a potential secret
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);

	auto secret_match = secret_manager.LookupSecret(transaction, path, secret_type);

	// No secret: nothing left to do here!
	if (!secret_match.HasMatch()) {
		if (StringUtil::StartsWith(path, "r2://") || StringUtil::StartsWith(path, "gs://") ||
		    StringUtil::StartsWith(path, "gcs://")) {
			throw NotImplementedException(
			    "Can not scan a gcs:// gs:// or r2:// url without a secret providing its endpoint currently. Please "
			    "create an R2 or GCS secret containing the credentials for this endpoint and try again.");
		}

		return builder;
	}
	const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_match.secret_entry->secret);

	KeyValueSecretReader secret_reader(kv_secret, *context.client_data->file_opener);

	// Here you would need to add the logic for setting the builder options for Azure
	// This is just a placeholder and will need to be replaced with the actual logic
	if (secret_type == "s3" || secret_type == "gcs" || secret_type == "r2") {
		string key_id, secret, session_token, region, endpoint, url_style;
		bool use_ssl = true;
		secret_reader.TryGetSecretKey("key_id", key_id);
		secret_reader.TryGetSecretKey("secret", secret);
		secret_reader.TryGetSecretKey("session_token", session_token);
		secret_reader.TryGetSecretKey("region", region);
		secret_reader.TryGetSecretKey("endpoint", endpoint);
		secret_reader.TryGetSecretKey("url_style", url_style);
		secret_reader.TryGetSecretKey("use_ssl", use_ssl);

		if (key_id.empty() && secret.empty()) {
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("skip_signature"),
			                        KernelUtils::ToDeltaString("true"));
		}

		if (!key_id.empty()) {
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("aws_access_key_id"),
			                        KernelUtils::ToDeltaString(key_id));
		}
		if (!secret.empty()) {
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("aws_secret_access_key"),
			                        KernelUtils::ToDeltaString(secret));
		}
		if (!session_token.empty()) {
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("aws_session_token"),
			                        KernelUtils::ToDeltaString(session_token));
		}
		if (!endpoint.empty() && endpoint != "s3.amazonaws.com") {
			if (!StringUtil::StartsWith(endpoint, "https://") && !StringUtil::StartsWith(endpoint, "http://")) {
				if (use_ssl) {
					endpoint = "https://" + endpoint;
				} else {
					endpoint = "http://" + endpoint;
				}
			}

			if (StringUtil::StartsWith(endpoint, "http://")) {
				ffi::set_builder_option(builder, KernelUtils::ToDeltaString("allow_http"),
				                        KernelUtils::ToDeltaString("true"));
			}
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("aws_endpoint"),
			                        KernelUtils::ToDeltaString(endpoint));
		} else if (StringUtil::StartsWith(path, "gs://") || StringUtil::StartsWith(path, "gcs://")) {
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("aws_endpoint"),
			                        KernelUtils::ToDeltaString("https://storage.googleapis.com"));
		}

		ffi::set_builder_option(builder, KernelUtils::ToDeltaString("aws_region"), KernelUtils::ToDeltaString(region));

	} else if (secret_type == "azure") {
		// azure seems to be super complicated as we need to cover duckdb azure plugin and delta RS builder
		// and both require different settings
		string connection_string, account_name, endpoint, client_id, client_secret, tenant_id, chain;
		secret_reader.TryGetSecretKey("connection_string", connection_string);
		secret_reader.TryGetSecretKey("account_name", account_name);
		secret_reader.TryGetSecretKey("endpoint", endpoint);
		secret_reader.TryGetSecretKey("client_id", client_id);
		secret_reader.TryGetSecretKey("client_secret", client_secret);
		secret_reader.TryGetSecretKey("tenant_id", tenant_id);
		secret_reader.TryGetSecretKey("chain", chain);

		if (!account_name.empty() && account_name == "onelake") {
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("use_fabric_endpoint"),
			                        KernelUtils::ToDeltaString("true"));
		}

		auto provider = kv_secret.GetProvider();
		if (provider == "access_token") {
			// Authentication option 0:
			// https://docs.rs/object_store/latest/object_store/azure/enum.AzureConfigKey.html#variant.Token
			string access_token;
			secret_reader.TryGetSecretKey("access_token", access_token);
			if (access_token.empty()) {
				throw InvalidInputException("No access_token value not found in secret provider!");
			}
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("bearer_token"),
			                        KernelUtils::ToDeltaString(access_token));
		} else if (provider == "credential_chain") {
			// Authentication option 1a: using the cli authentication
			if (chain.find("cli") != std::string::npos) {
				ffi::set_builder_option(builder, KernelUtils::ToDeltaString("use_azure_cli"),
				                        KernelUtils::ToDeltaString("true"));
			}
			// Authentication option 1b: non-cli credential chains will just "hope for the best" technically since we
			// are using the default credential chain provider duckDB and delta-kernel-rs should find the same auth
		} else if (!connection_string.empty() && connection_string != "NULL") {

			// Authentication option 2: a connection string based on account key
			auto account_key = parseFromConnectionString(connection_string, "AccountKey");
			account_name = parseFromConnectionString(connection_string, "AccountName");
			// Authentication option 2: a connection string based on account key
			if (!account_name.empty() && !account_key.empty()) {
				ffi::set_builder_option(builder, KernelUtils::ToDeltaString("account_key"),
				                        KernelUtils::ToDeltaString(account_key));
			} else {
				// Authentication option 2b: a connection string based on SAS token
				endpoint = parseFromConnectionString(connection_string, "BlobEndpoint");
				if (account_name.empty()) {
					account_name = ParseAccountNameFromEndpoint(endpoint);
				}
				auto sas_token = parseFromConnectionString(connection_string, "SharedAccessSignature");
				if (!sas_token.empty()) {
					ffi::set_builder_option(builder, KernelUtils::ToDeltaString("sas_token"),
					                        KernelUtils::ToDeltaString(sas_token));
				}
			}
		} else if (provider == "service_principal") {
			if (!client_id.empty()) {
				ffi::set_builder_option(builder, KernelUtils::ToDeltaString("azure_client_id"),
				                        KernelUtils::ToDeltaString(client_id));
			}
			if (!client_secret.empty()) {
				ffi::set_builder_option(builder, KernelUtils::ToDeltaString("azure_client_secret"),
				                        KernelUtils::ToDeltaString(client_secret));
			}
			if (!tenant_id.empty()) {
				ffi::set_builder_option(builder, KernelUtils::ToDeltaString("azure_tenant_id"),
				                        KernelUtils::ToDeltaString(tenant_id));
			}
		} else {
			// Authentication option 3: no authentication, just an account name
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("azure_skip_signature"),
			                        KernelUtils::ToDeltaString("true"));
		}
		// Set the use_emulator option for when the azurite test server is used
		if (account_name == "devstoreaccount1" || connection_string.find("devstoreaccount1") != string::npos) {
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("use_emulator"),
			                        KernelUtils::ToDeltaString("true"));
		}
		if (!account_name.empty()) {
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("account_name"),
			                        KernelUtils::ToDeltaString(account_name)); // needed for delta RS builder
		}
		if (!endpoint.empty()) {
			ffi::set_builder_option(builder, KernelUtils::ToDeltaString("azure_endpoint"),
			                        KernelUtils::ToDeltaString(endpoint));
		}
		ffi::set_builder_option(builder, KernelUtils::ToDeltaString("container_name"),
		                        KernelUtils::ToDeltaString(bucket));
	}
	return builder;
}

struct KernelPartitionVisitorData {
	vector<string> partitions;
	ErrorData err_data;
};

static void KernelPartitionStringVisitor(ffi::NullableCvoid engine_context, ffi::KernelStringSlice slice) {
	auto data = static_cast<KernelPartitionVisitorData *>(engine_context);
	data->partitions.push_back(KernelUtils::FromDeltaString(slice));
}

static Value GetPartitionValueFromExpression(const vector<unique_ptr<ParsedExpression>> &parsed_expression,
                                             idx_t index) {
	auto &column_expressions = KernelUtils::UnpackTopLevelStruct(parsed_expression);
	auto &child = column_expressions[index];
	if (!child || child->type != ExpressionType::VALUE_CONSTANT) {
		throw IOException("Failed to parse partition value from kernel-provided transformation");
	}
	return child->Cast<ConstantExpression>().value;
}

void ScanDataCallBack::VisitCallbackInternal(ffi::NullableCvoid engine_context, ffi::KernelStringSlice path,
                                             int64_t size, const ffi::Stats *stats, const ffi::DvInfo *dv_info,
                                             const ffi::Expression *transform) {
	auto context = (ScanDataCallBack *)engine_context;
	auto &snapshot = context->snapshot;

	auto path_string = snapshot.GetPath();
	StringUtil::RTrim(path_string, "/");
	path_string += "/" + KernelUtils::FromDeltaString(path);

	path_string = url_decode(path_string);

	// First we append the file to our resolved files
	snapshot.resolved_files.emplace_back(DeltaMultiFileList::ToDuckDBPath(path_string));
	snapshot.metadata.emplace_back(make_uniq<DeltaFileMetaData>());

	D_ASSERT(snapshot.resolved_files.size() == snapshot.metadata.size());

	// Initialize the file metadata
	snapshot.metadata.back()->delta_snapshot_version = snapshot.version;
	snapshot.metadata.back()->file_number = snapshot.resolved_files.size() - 1;
	if (stats) {
		snapshot.metadata.back()->cardinality = stats->num_records;
	}

	// Fetch the deletion vector
	auto selection_vector_res =
	    ffi::selection_vector_from_dv(dv_info, snapshot.extern_engine.get(), snapshot.global_state.get());

	// TODO: remove workaround for https://github.com/duckdb/duckdb-delta/issues/150
	bool do_workaround = false;
	if (selection_vector_res.tag == ffi::ExternResult<ffi::KernelBoolSlice>::Tag::Err && selection_vector_res.err._0) {
		auto error_cast = static_cast<DuckDBEngineError *>(selection_vector_res.err._0);
		if (error_cast->error_message == "Deletion Vector error: Unknown storage format: ''.") {
			do_workaround = true;
		}
	}

	if (!do_workaround) {
		ffi::KernelBoolSlice selection_vector;
		auto res = KernelUtils::TryUnpackResult(selection_vector_res, selection_vector);
		if (res.HasError()) {
			context->error = res;
			return;
		}
		if (selection_vector.ptr) {
			snapshot.metadata.back()->selection_vector = selection_vector;
		}
	}

	// Lookup all columns for potential hits in the constant map
	if (transform) {
		ExpressionVisitor visitor;
		auto parsed_transformation_expression = visitor.VisitKernelExpression(transform);

		if (!parsed_transformation_expression) {
			context->error = ErrorData(ExceptionType::IO,
			                           "Failed to parse transformation expression from delta kernel: null returned");
			return;
		}

		case_insensitive_map_t<Value> constant_map;
		for (idx_t i = 0; i < snapshot.partitions.size(); ++i) {
			const auto &partition_id = context->snapshot.partition_ids[i];
			const auto &partition_name = context->snapshot.partitions[i];

			constant_map[partition_name] =
			    GetPartitionValueFromExpression(*parsed_transformation_expression, partition_id);
		}
		snapshot.metadata.back()->partition_map = std::move(constant_map);
		snapshot.metadata.back()->transform_expression =
		    std::move(parsed_transformation_expression); // FIXME: currently not used
	} else {
		if (!snapshot.partitions.empty()) {
			context->error = ErrorData(ExceptionType::IO,
			                           "Failed to fetch partitions from delta kernel transform! Transform is empty");
			return;
		}
	}
}

void ScanDataCallBack::VisitCallback(ffi::NullableCvoid engine_context, ffi::KernelStringSlice path, int64_t size,
                                     const ffi::Stats *stats, const ffi::DvInfo *dv_info,
                                     const ffi::Expression *transform, const ffi::CStringMap *partition_values) {
	try {
		return VisitCallbackInternal(engine_context, path, size, stats, dv_info, transform);
	} catch (std::runtime_error &e) {
		auto context = (ScanDataCallBack *)engine_context;
		context->error = ErrorData(e);
	}
}

void ScanDataCallBack::VisitData(ffi::NullableCvoid engine_context, ffi::Handle<ffi::SharedScanMetadata> scan_metadata) {
	ffi::visit_scan_metadata(scan_metadata, engine_context, VisitCallback);
}

DeltaMultiFileList::DeltaMultiFileList(ClientContext &context_p, const string &path)
    : MultiFileList({ToDeltaPath(path)}, FileGlobOptions::ALLOW_EMPTY), context(context_p) {
}

string DeltaMultiFileList::GetPath() const {
	return GetPaths()[0].path;
}

string DeltaMultiFileList::ToDuckDBPath(const string &raw_path) {
	if (StringUtil::StartsWith(raw_path, "file://")) {
		return raw_path.substr(7);
	}
	return raw_path;
}

string DeltaMultiFileList::ToDeltaPath(const string &raw_path) {
	string path;
	if (StringUtil::StartsWith(raw_path, "./")) {
		LocalFileSystem fs;
		path = fs.JoinPath(fs.GetWorkingDirectory(), raw_path.substr(2));
		path = "file://" + path;
	} else {
		path = raw_path;
	}

	// Paths always end in a slash (kernel likes it that way for now)
	if (path[path.size() - 1] != '/') {
		path = path + '/';
	}

	return path;
}

void DeltaMultiFileList::Bind(vector<LogicalType> &return_types, vector<string> &names) {
	unique_lock<mutex> lck(lock);

	if (have_bound) {
		names = this->names;
		return_types = this->types;
		return;
	}

	EnsureSnapshotInitialized();

	unique_ptr<SchemaVisitor::FieldList> schema;
	{
		auto snapshot_ref = snapshot->GetLockingRef();
		schema = SchemaVisitor::VisitSnapshotSchema(snapshot_ref.GetPtr());
	}

	for (const auto &field : *schema) {
		names.push_back(field.first);
		return_types.push_back(field.second);
	}
	// Store the bound names for resolving the complex filter pushdown later
	have_bound = true;
	this->names = names;
	this->types = return_types;
}

OpenFileInfo DeltaMultiFileList::GetFileInternal(idx_t i) const {
	EnsureScanInitialized();

	// We already have this file
	if (i < resolved_files.size()) {
		return resolved_files[i];
	}

	if (files_exhausted) {
		return OpenFileInfo();
	}

	ScanDataCallBack callback_context(*this);

	while (i >= resolved_files.size()) {
		auto have_scan_data_res =
		    ffi::scan_metadata_next(scan_data_iterator.get(), &callback_context, ScanDataCallBack::VisitData);

		if (callback_context.error.HasError()) {
			callback_context.error.Throw();
		}

		bool have_scan_data;
		auto scan_data_res = KernelUtils::TryUnpackResult<bool>(have_scan_data_res, have_scan_data);
		if (scan_data_res.HasError()) {
			throw IOException("Failed to unpack scan data from kernel: %s", scan_data_res.RawMessage());
		}

		// kernel has indicated that we have no more data to scan
		if (!have_scan_data) {
			files_exhausted = true;
			return OpenFileInfo();
		}
	}

	return resolved_files[i];
}

idx_t DeltaMultiFileList::GetTotalFileCountInternal() const {
	idx_t i = resolved_files.size();
	while (!GetFileInternal(i).path.empty()) {
		i++;
	}
	return resolved_files.size();
}

OpenFileInfo DeltaMultiFileList::GetFile(idx_t i) {
	// TODO: profile this: we should be able to use atomics here to optimize
	unique_lock<mutex> lck(lock);
	return GetFileInternal(i);
}

void DeltaMultiFileList::InitializeSnapshot() const {
	auto path_slice = KernelUtils::ToDeltaString(paths[0].path);

	auto interface_builder = CreateBuilder(context, paths[0].path);
	extern_engine = TryUnpackKernelResult(ffi::builder_build(interface_builder));

	if (!snapshot) {
		snapshot = make_shared_ptr<SharedKernelSnapshot>(
		    TryUnpackKernelResult(ffi::snapshot(path_slice, extern_engine.get())));
	}

	// Set version
	auto snapshot_ref = snapshot->GetLockingRef();
	this->version = ffi::version(snapshot_ref.GetPtr());

	initialized_snapshot = true;
}

static void InjectColumnIdentifiers(const vector<string> &names, const vector<LogicalType> &types,
                                    const vector<string> &all_names, const vector<LogicalType> &all_types,
                                    vector<MultiFileColumnDefinition> &global_column_defs) {
	for (idx_t i = 0; i < names.size(); i++) {
		auto &col = global_column_defs[i];
		col.default_expression = make_uniq<ConstantExpression>(Value(col.type));
		col.identifier = Value(all_names[i]);

		if (col.type.id() == LogicalTypeId::STRUCT) {
			vector<string> child_names;
			vector<LogicalType> child_types;
			for (idx_t j = 0; j < StructType::GetChildCount(col.type); j++) {
				child_names.emplace_back(StructType::GetChildName(col.type, j));
				child_types.emplace_back(StructType::GetChildType(col.type, j));
			}

			vector<string> child_all_names;
			vector<LogicalType> child_all_types;
			for (idx_t j = 0; j < StructType::GetChildCount(all_types[i]); j++) {
				child_all_names.emplace_back(StructType::GetChildName(all_types[i], j));
				child_all_types.emplace_back(StructType::GetChildType(all_types[i], j));
			}

			InjectColumnIdentifiers(child_names, child_types, child_all_names, child_all_types, col.children);
		}
	}
}

static vector<MultiFileColumnDefinition> ConstructGlobalColDefs(const vector<string> &names,
                                                                const vector<LogicalType> &types,
                                                                const vector<string> &partitions,
                                                                ffi::SharedGlobalScanState *scan_state) {
	vector<string> physical_names;
	vector<LogicalType> physical_types;
	vector<string> logical_names;
	vector<LogicalType> logical_types;
	unordered_map<string, string> name_map;
	unordered_map<string, LogicalType> physical_type_map;
	unordered_set<string> partition_set;

	for (const auto &partition : partitions) {
		partition_set.insert(partition);
	}

	auto schema_physical = SchemaVisitor::VisitSnapshotGlobalReadSchema(scan_state, false);
	auto schema_logical = SchemaVisitor::VisitSnapshotGlobalReadSchema(scan_state, true);

	for (idx_t i = 0; i < schema_physical->size(); i++) {
		physical_names.push_back((*schema_physical)[i].first);
		physical_types.push_back((*schema_physical)[i].second);
	}
	for (idx_t i = 0; i < schema_logical->size(); i++) {
		logical_names.push_back((*schema_logical)[i].first);
		logical_types.push_back((*schema_logical)[i].second);
	}

	idx_t physical_idx = 0;
	for (idx_t i = 0; i < logical_names.size(); i++) {
		auto &logical_name = logical_names[i];
		if (partition_set.find(logical_name) != partition_set.end()) {
			continue;
		}
		if (physical_idx >= physical_names.size()) {
			throw IOException("Failed to map physical schema to logical");
		}
		name_map[logical_names[i]] = physical_names[physical_idx];
		physical_type_map[logical_names[i]] = physical_types[physical_idx];
		physical_idx++;
	}

	vector<string> all_names;
	vector<LogicalType> all_types;
	for (idx_t i = 0; i < names.size(); i++) {
		auto &name = names[i];
		auto &type = types[i];

		auto lu = name_map.find(name);
		if (lu != name_map.end()) {
			all_names.push_back(lu->second);
			all_types.push_back(physical_type_map[name]);
		} else {
			all_names.push_back(name);
			all_types.push_back(type);
		}
	}

	auto global_column_defs = MultiFileColumnDefinition::ColumnsFromNamesAndTypes(names, types);

	InjectColumnIdentifiers(names, types, all_names, all_types, global_column_defs);

	return global_column_defs;
}

void DeltaMultiFileList::InitializeScan() const {
	auto snapshot_ref = snapshot->GetLockingRef();

	// Create Scan
	PredicateVisitor visitor(names, &table_filters);
	scan = TryUnpackKernelResult(ffi::scan(snapshot_ref.GetPtr(), extern_engine.get(), &visitor));

	if (visitor.error_data.HasError()) {
		throw IOException("Failed to initialize Scan for Delta table at '%s'. Original error: '%s'", paths[0].path,
		                  visitor.error_data.Message());
	}

	// Create GlobalState
	global_state = ffi::get_global_scan_state(scan.get());

	// Create scan data iterator
	scan_data_iterator = TryUnpackKernelResult(ffi::scan_metadata_iter_init(extern_engine.get(), scan.get()));

	// Load partitions
	auto partition_count = ffi::get_partition_column_count(snapshot_ref.GetPtr());
	if (partition_count > 0) {
		auto string_slice_iterator = ffi::get_partition_columns(snapshot_ref.GetPtr());

		KernelPartitionVisitorData data;
		while (string_slice_next(string_slice_iterator, &data, KernelPartitionStringVisitor)) {
		}
		partitions = data.partitions;

		for (auto &partition : partitions) {
			for (idx_t i = 0; i < names.size(); i++) {
				if (partition == names[i]) {
					partition_ids.push_back(i);
					break;
				}
			}
		}

		if (partitions.size() != partition_ids.size()) {
			throw IOException("Failed to map partitions to columns");
		}
	}

	lazy_loaded_schema = ConstructGlobalColDefs(names, types, partitions, global_state.get());

	initialized_scan = true;
}

void DeltaMultiFileList::EnsureSnapshotInitialized() const {
	if (!initialized_snapshot) {
		InitializeSnapshot();
	}
}

void DeltaMultiFileList::EnsureScanInitialized() const {
	EnsureSnapshotInitialized();
	if (!initialized_scan) {
		InitializeScan();
	}
}

unique_ptr<DeltaMultiFileList> DeltaMultiFileList::PushdownInternal(ClientContext &context,
                                                                    TableFilterSet &new_filters) const {
	auto filtered_list = make_uniq<DeltaMultiFileList>(context, paths[0].path);

	TableFilterSet result_filter_set;

	// Add pre-existing filters
	for (auto &entry : table_filters.filters) {
		result_filter_set.PushFilter(ColumnIndex(entry.first), entry.second->Copy());
	}

	// Add new filters
	for (auto &entry : new_filters.filters) {
		if (entry.first < names.size()) {
			result_filter_set.PushFilter(ColumnIndex(entry.first), entry.second->Copy());
		}
	}

	filtered_list->table_filters = std::move(result_filter_set);
	filtered_list->names = names;
	filtered_list->types = types;
	filtered_list->lazy_loaded_schema = lazy_loaded_schema;

	// Copy over the snapshot, this avoids reparsing metadata
	{
		unique_lock<mutex> lck(lock);
		filtered_list->snapshot = snapshot;
	}

	return filtered_list;
}

static DeltaFilterPushdownMode GetDeltaFilterPushdownMode(ClientContext &context, const MultiFileOptions &options) {
	auto res = options.custom_options.find("pushdown_filters");
	if (res != options.custom_options.end()) {
		auto str = res->second.GetValue<string>();
		return DeltaEnumUtils::FromString(str);
	}

	return DEFAULT_PUSHDOWN_MODE;
}
unique_ptr<MultiFileList> DeltaMultiFileList::ComplexFilterPushdown(ClientContext &context,
                                                                    const MultiFileOptions &options,
                                                                    MultiFilePushdownInfo &info,
                                                                    vector<unique_ptr<Expression>> &filters) {
	auto pushdown_mode = GetDeltaFilterPushdownMode(context, options);
	if (pushdown_mode == DeltaFilterPushdownMode::NONE || pushdown_mode == DeltaFilterPushdownMode::DYNAMIC_ONLY) {
		return nullptr;
	}

	FilterCombiner combiner(context);

	if (filters.empty()) {
		return nullptr;
	}

	for (auto riter = filters.rbegin(); riter != filters.rend(); ++riter) {
		combiner.AddFilter(riter->get()->Copy());
	}

	vector<FilterPushdownResult> pushdown_results;
	auto filter_set = combiner.GenerateTableScanFilters(info.column_indexes, pushdown_results);
	if (filter_set.filters.empty()) {
		return nullptr;
	}

	auto filtered_list = PushdownInternal(context, filter_set);

	ReportFilterPushdown(context, *filtered_list, info.column_ids, "constant", info);

	return std::move(filtered_list);
}

void DeltaMultiFileList::ReportFilterPushdown(ClientContext &context, DeltaMultiFileList &new_list,
                                              const vector<column_t> &column_ids, const char *pushdown_type,
                                              optional_ptr<MultiFilePushdownInfo> mfr_info) const {
	auto &logger = Logger::Get(context);
	auto log_level = LogLevel::LOG_INFO;
	auto delta_log_type = "delta.FilterPushdown";

	// This function both reports the filter pushdown to the explain output (regular pushdown only) and the logger (both
	// regular and dynamic)
	bool should_log = logger.ShouldLog(delta_log_type, log_level);
	bool should_report_explain_output = mfr_info != nullptr && QueryProfiler::Get(context).IsEnabled();

	// We should neither log, nor report explain output: we're done here!
	if (!should_report_explain_output && !should_log) {
		return;
	}

	Value result;
	if (!context.TryGetCurrentSetting("delta_scan_explain_files_filtered", result)) {
		throw InternalException("Failed to find 'delta_scan_explain_files_filtered' option!");
	}
	bool delta_scan_explain_files_filtered = result.GetValue<bool>();

	// Report the filter counts
	idx_t old_total = DConstants::INVALID_INDEX;
	idx_t new_total = DConstants::INVALID_INDEX;
	if (delta_scan_explain_files_filtered) {
		// FIXME: This weird call is due to the MultiFileReader::GetTotalFileCount method being non const: API should be
		// reworked to clean this up
		{
			unique_lock<mutex> lck(lock);
			EnsureScanInitialized();
			old_total = GetTotalFileCountInternal();
		}
		new_total = new_list.GetTotalFileCount();

		if (should_report_explain_output) {
			if (!mfr_info->extra_info.total_files.IsValid()) {
				mfr_info->extra_info.total_files = old_total;
			} else if (mfr_info->extra_info.total_files.GetIndex() != old_total) {
				throw InternalException(
				    "Error encountered when analyzing filtered out files for delta scan: total_files inconsistent!");
			}

			if (!mfr_info->extra_info.filtered_files.IsValid() ||
			    mfr_info->extra_info.filtered_files.GetIndex() >= new_total) {
				mfr_info->extra_info.filtered_files = new_total;
			} else {
				throw InternalException(
				    "Error encountered when analyzing filtered out files for delta scan: filtered_files inconsistent!");
			}
		}
	}

	// Report the new filters
	vector<Value> old_filters_value_list;
	for (auto &f : table_filters.filters) {
		auto &column_index = f.first;
		auto &filter = f.second;
		if (column_index < names.size()) {
			auto &col_name = names[column_index];
			old_filters_value_list.push_back(filter->ToString(col_name));
		}
	}
	auto old_filters_value = Value::LIST(LogicalType::VARCHAR, old_filters_value_list);

	// Report the new filters
	vector<Value> filters_value_list;
	for (auto &f : new_list.table_filters.filters) {
		auto &column_index = f.first;
		auto &filter = f.second;
		if (column_index < names.size()) {
			auto &col_name = names[column_index];
			filters_value_list.push_back(filter->ToString(col_name));
		}
	}
	auto filters_value = Value::LIST(LogicalType::VARCHAR, filters_value_list);

	if (should_report_explain_output) {
		string files_string;
		for (auto &filter : filters_value_list) {
			files_string += filter.ToString() + "\n";
		}
		mfr_info->extra_info.file_filters = files_string.substr(0, files_string.size() - 1);
	}

	if (should_log) {
		child_list_t<Value> struct_fields;
		struct_fields.push_back({"path", Value(GetPath())});
		struct_fields.push_back({"type", Value(pushdown_type)});
		struct_fields.push_back({"filters_before", old_filters_value});
		struct_fields.push_back({"filters_after", filters_value});
		if (new_total != DConstants::INVALID_INDEX) {
			struct_fields.push_back({"files_before", Value::BIGINT(old_total)});
			struct_fields.push_back({"files_after", Value::BIGINT(new_total)});
		}
		auto struct_value = Value::STRUCT(struct_fields);
		logger.WriteLog(delta_log_type, log_level, struct_value.ToString());
	}
}

unique_ptr<MultiFileList>
DeltaMultiFileList::DynamicFilterPushdown(ClientContext &context, const MultiFileOptions &options,
                                          const vector<string> &names, const vector<LogicalType> &types,
                                          const vector<column_t> &column_ids, TableFilterSet &filters) const {
	auto pushdown_mode = GetDeltaFilterPushdownMode(context, options);
	if (pushdown_mode == DeltaFilterPushdownMode::NONE || pushdown_mode == DeltaFilterPushdownMode::CONSTANT_ONLY) {
		return nullptr;
	}

	if (filters.filters.empty()) {
		return nullptr;
	}

	TableFilterSet filters_copy;
	for (auto &filter : filters.filters) {
		auto column_id = column_ids[filter.first];
		auto previously_pushed_down_filter = this->table_filters.filters.find(column_id);
		if (previously_pushed_down_filter != this->table_filters.filters.end() &&
		    filter.second->Equals(*previously_pushed_down_filter->second)) {
			// Skip filters that we already have pushed down
			continue;
		}
		filters_copy.PushFilter(ColumnIndex(column_id), filter.second->Copy());
	}

	if (!filters_copy.filters.empty()) {
		auto new_snap = PushdownInternal(context, filters_copy);
		ReportFilterPushdown(context, *new_snap, column_ids, "dynamic", nullptr);
		return std::move(new_snap);
	}

	return nullptr;
}

vector<OpenFileInfo> DeltaMultiFileList::GetAllFiles() {
	unique_lock<mutex> lck(lock);
	idx_t i = resolved_files.size();
	// TODO: this can probably be improved
	while (!GetFileInternal(i).path.empty()) {
		i++;
	}
	return resolved_files;
}

FileExpandResult DeltaMultiFileList::GetExpandResult() {
	// We avoid exposing the ExpandResult to DuckDB here because we want to materialize the Snapshot as late as
	// possible: materializing too early (GetExpandResult is called *before* filter pushdown by the Parquet scanner),
	// will lead into needing to create 2 scans of the snapshot TODO: we need to investigate if this is actually a
	// sensible decision with some benchmarking, its currently based on intuition.
	return FileExpandResult::MULTIPLE_FILES;
}

idx_t DeltaMultiFileList::GetTotalFileCount() {
	unique_lock<mutex> lck(lock);
	return GetTotalFileCountInternal();
}

unique_ptr<NodeStatistics> DeltaMultiFileList::GetCardinality(ClientContext &context) {
	// This also ensures all files are expanded
	auto total_file_count = DeltaMultiFileList::GetTotalFileCount();

	// TODO: internalize above
	unique_lock<mutex> lck(lock);

	if (total_file_count == 0) {
		return make_uniq<NodeStatistics>(0, 0);
	}

	idx_t total_tuple_count = 0;
	bool have_any_stats = false;
	for (auto &metadatum : metadata) {
		if (metadatum->cardinality != DConstants::INVALID_INDEX) {
			have_any_stats = true;
			total_tuple_count += metadatum->cardinality;
		}
	}

	if (have_any_stats) {
		return make_uniq<NodeStatistics>(total_tuple_count, total_tuple_count);
	}

	return nullptr;
}

idx_t DeltaMultiFileList::GetVersion() {
	unique_lock<mutex> lck(lock);
	EnsureSnapshotInitialized();
	return version;
}

DeltaFileMetaData &DeltaMultiFileList::GetMetaData(idx_t index) const {
	unique_lock<mutex> lck(lock);
	if (index >= metadata.size()) {
		throw InternalException("Attempted to fetch metadata for nonexistent file in DeltaMultiFileList");
	}
	return *metadata[index];
}

vector<string> DeltaMultiFileList::GetPartitionColumns() {
	unique_lock<mutex> lck(lock);
	EnsureScanInitialized();
	return partitions;
}

vector<MultiFileColumnDefinition> &DeltaMultiFileList::GetLazyLoadedGlobalColumns() const {
	unique_lock<mutex> lck(lock);
	EnsureScanInitialized();
	return lazy_loaded_schema;
}

unique_ptr<MultiFileReader> DeltaMultiFileReader::CreateInstance(const TableFunction &table_function) {
	auto result = make_uniq<DeltaMultiFileReader>();

	if (table_function.function_info) {
		result->snapshot = table_function.function_info->Cast<DeltaFunctionInfo>().snapshot;
	}

	return std::move(result);
}

} // namespace duckdb
