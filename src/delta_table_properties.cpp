#include "delta_table_properties.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

//! Kernel v0.30.1, kernel/src/table_properties/mod.rs (the keys deserialize.rs parses)
static const char *const KERNEL_TABLE_PROPERTIES[] = {
    "delta.appendOnly",
    "delta.autoOptimize.autoCompact",
    "delta.autoOptimize.optimizeWrite",
    "delta.checkpointInterval",
    "delta.checkpoint.writeStatsAsJson",
    "delta.checkpoint.writeStatsAsStruct",
    "delta.columnMapping.mode",
    "delta.columnMapping.maxColumnId",
    "delta.dataSkippingNumIndexedCols",
    "delta.dataSkippingStatsColumns",
    "delta.deletedFileRetentionDuration",
    "delta.enableChangeDataFeed",
    "delta.enableDeletionVectors",
    "delta.enableTypeWidening",
    "delta.enableIcebergCompatV1",
    "delta.enableIcebergCompatV2",
    "delta.enableIcebergCompatV3",
    "delta.isolationLevel",
    "delta.logRetentionDuration",
    "delta.enableExpiredLogCleanup",
    "delta.randomizeFilePrefixes",
    "delta.randomPrefixLength",
    "delta.setTransactionRetentionDuration",
    "delta.targetFileSize",
    "delta.tuneFileSizesForRewrites",
    "delta.checkpointPolicy",
    "delta.enableRowTracking",
    "delta.rowTracking.materializedRowIdColumnName",
    "delta.rowTracking.materializedRowCommitVersionColumnName",
    "delta.rowTrackingSuspended",
    "delta.parquet.format.version",
    "delta.parquet.compression.codec",
    "delta.enableInCommitTimestamps",
    "delta.inCommitTimestampEnablementVersion",
    "delta.inCommitTimestampEnablementTimestamp",
};

//! Not a kernel property, but duckdb-delta reads it (it allows shredded VARIANT files), so kernel is the one to
//! refuse it at create
static const char *const DUCKDB_DELTA_TABLE_PROPERTIES[] = {
    "delta.enableVariantShredding",
};

//! Kernel v0.30.1, kernel/src/table_features/mod.rs (TableFeature's names)
static const char *const KERNEL_TABLE_FEATURES[] = {
    "appendOnly",
    "invariants",
    "checkConstraints",
    "changeDataFeed",
    "generatedColumns",
    "identityColumns",
    "inCommitTimestamp",
    "rowTracking",
    "domainMetadata",
    "icebergCompatV1",
    "icebergCompatV2",
    "icebergCompatV3",
    "icebergNativeV4-preview",
    "clustering",
    "materializePartitionColumns",
    "catalogManaged",
    "catalogOwned-preview",
    "columnMapping",
    "deletionVectors",
    "timestampNtz",
    "typeWidening",
    "typeWidening-preview",
    "v2Checkpoint",
    "vacuumProtocolCheck",
    "variantType",
    "variantType-preview",
    "variantShredding",
    "variantShredding-preview",
    "adaptiveMetadata-preview",
    "allowColumnDefaults",
    "geospatial",
};

static const char *const PROTOCOL_PROPERTIES[] = {
    "delta.minReaderVersion",
    "delta.minWriterVersion",
};

const vector<string> &DeltaTableProperties::KnownKeys() {
	static const vector<string> keys = [] {
		vector<string> result;
		for (auto key : KERNEL_TABLE_PROPERTIES) {
			result.emplace_back(key);
		}
		for (auto key : DUCKDB_DELTA_TABLE_PROPERTIES) {
			result.emplace_back(key);
		}
		for (auto feature : KERNEL_TABLE_FEATURES) {
			result.emplace_back(string("delta.feature.") + feature);
		}
		for (auto key : PROTOCOL_PROPERTIES) {
			result.emplace_back(key);
		}
		return result;
	}();
	return keys;
}

bool DeltaTableProperties::IsDeltaKey(const string &key) {
	return StringUtil::StartsWith(StringUtil::Lower(key), "delta.");
}

optional<string> DeltaTableProperties::FindKnownKey(const string &key) {
	for (auto &known : KnownKeys()) {
		if (StringUtil::CIEquals(known, key)) {
			return known;
		}
	}
	return optional<string>();
}

void DeltaTableProperties::ThrowIfUnknownDeltaKey(const string &key, const string &statement) {
	if (!IsDeltaKey(key)) {
		return;
	}
	auto known = FindKnownKey(key);
	if (known && *known == key) {
		return;
	}
	if (known) {
		throw InvalidInputException("%s: Delta table property keys are case-sensitive: '%s' is spelled '%s'", statement,
		                            key, *known);
	}
	auto candidates = StringUtil::CandidatesMessage(StringUtil::TopNJaroWinkler(KnownKeys(), key, 3), "Did you mean");
	throw InvalidInputException("%s: unknown Delta table property '%s'\n%s", statement, key, candidates);
}

} // namespace duckdb
