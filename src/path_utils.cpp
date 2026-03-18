#include "path_utils.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

//
// TODO: (@benfleis) after ToLocal/ToFileLocal/GetCommonLineage land in duckdb core, delete this file.
//

namespace duckdb {

Path PathToLocal(const Path &path) {
	if (!path.IsLocal()) {
		throw InvalidInputException("Path: cannot convert non-local path to local: %s", path.ToString());
	}
	// Strip scheme + authority, keep anchor + segments
	return Path::FromString(path.GetAnchor() + path.GetPath() + path.GetTrailingSeparator());
}

Path PathToFileLocal(const Path &path) {
	if (!path.IsLocal()) {
		throw InvalidInputException("Path: cannot convert non-local path to file-local: %s", path.ToString());
	}
	// Build file:// + anchor + path; anchor already starts with "/"
	return Path::FromString("file://" + path.GetAnchor() + path.GetPath() + path.GetTrailingSeparator());
}

int PathGetCommonLineage(const Path &path, const Path &ancestor, int min_depth) {
	if (path.GetScheme() != ancestor.GetScheme() || path.GetAuthority() != ancestor.GetAuthority() ||
	    path.GetAnchor() != ancestor.GetAnchor()) {
		return -1;
	}
	const auto &psegs = path.GetPathSegments();
	const auto &asegs = ancestor.GetPathSegments();
	if (asegs.size() > psegs.size()) {
		return -1;
	}
	int depth = static_cast<int>(psegs.size() - asegs.size());
	if (min_depth >= 0 && depth < min_depth) {
		return -1;
	}
#if defined(_WIN32)
	if ((path.GetSeparator() == '\\') || path.HasDrive()) {
		return std::equal(asegs.begin(), asegs.end(), psegs.begin(),
		                  [](const string &a, const string &b) { return StringUtil::CIEquals(a, b); })
		           ? depth
		           : -1;
	}
#endif
	return std::equal(asegs.begin(), asegs.end(), psegs.begin()) ? depth : -1;
}

} // namespace duckdb
