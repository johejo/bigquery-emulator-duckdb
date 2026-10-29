#include "src/gcs_uri.h"

#include <cstddef>
#include <string>

#include "src/api_error.h"

namespace bigquery_emulator_duckdb {

GcsObjectUri ParseGcsUri(const std::string& uri) {
  const size_t slash = uri.find('/', 5);
  if (!uri.starts_with("gs://") || slash == std::string::npos || slash == 5 ||
      slash + 1 == uri.size()) {
    throw ApiError::Invalid("Invalid GCS URI: " + uri);
  }
  if (uri.substr(5, slash - 5).find('*') != std::string::npos) {
    throw ApiError::Invalid("Wildcard is not allowed in a bucket name: " + uri);
  }
  return {.bucket = uri.substr(5, slash - 5), .object = uri.substr(slash + 1)};
}

size_t FindGcsWildcard(const std::string& uri) {
  const size_t wildcard = uri.find('*', uri.size() - ParseGcsUri(uri).object.size());
  if (wildcard != std::string::npos && uri.find('*', wildcard + 1) != std::string::npos) {
    throw ApiError::Invalid("Only one wildcard is allowed in a GCS URI: " + uri);
  }
  return wildcard;
}

}  // namespace bigquery_emulator_duckdb
