#pragma once

#include <cstddef>
#include <string>

namespace bigquery_emulator_duckdb {

struct GcsObjectUri {
  std::string bucket;
  std::string object;
};

// Pure URI validation, shared by metadata handling and acquisition. Throws ApiError for an
// invalid gs:// URI or a wildcard in the bucket name.
GcsObjectUri ParseGcsUri(const std::string& uri);

// The position of the wildcard in the gs:// URI `uri`, or npos when it has none. Loads and
// extracts alike allow one `*`, in the object name. Throws ApiError for a malformed URI, a
// wildcard in the bucket name or more than one wildcard.
// third_party/bigquery_docs/batch-loading-data.md
// third_party/bigquery_docs/exporting-data.md
size_t FindGcsWildcard(const std::string& uri);

}  // namespace bigquery_emulator_duckdb
