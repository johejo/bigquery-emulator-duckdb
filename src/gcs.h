#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bigquery_emulator_duckdb {

// The position of the wildcard in the gs:// URI `uri`, or npos when it has none. Loads and
// extracts alike allow one `*`, in the object name. Throws ApiError for a malformed URI, a
// wildcard in the bucket name or more than one wildcard.
// third_party/bigquery_docs/batch-loading-data.md
// third_party/bigquery_docs/exporting-data.md
size_t FindGcsWildcard(const std::string& uri);

// The metadata of a GCS object version.
struct GcsObjectMetadata {
  std::string content_type;
  // Base64, as GCS gives it; empty for objects without one, such as composite objects.
  std::string md5_hash;
  std::uint64_t size = 0;
  std::int64_t generation = 0;
  std::chrono::system_clock::time_point updated;
};

// Retains SDK connection pools and credentials for the lifetime of the emulator. Concurrent
// downloads and uploads are supported.
class GcsClient {
 public:
  // Reads the endpoint from the environment: CLOUD_STORAGE_EMULATOR_ENDPOINT or
  // CLOUD_STORAGE_TESTBENCH_ENDPOINT, which the SDK itself honors, then STORAGE_EMULATOR_HOST,
  // and otherwise the public Storage API. Any of the variables selects an emulator.
  GcsClient();
  // Emulators are always read anonymously. Other endpoints use Application Default Credentials
  // when they are available and anonymous access otherwise.
  GcsClient(std::string endpoint, bool emulator);
  ~GcsClient();

  // The Storage API endpoint downloads read from.
  [[nodiscard]] const std::string& endpoint() const;

  // Expands one object-name wildcard; exact URIs do not require listing permission.
  // Throws ApiError for malformed patterns, listing failures, or no matches.
  std::vector<std::string> Expand(const std::string& uri);

  // The metadata of the current version of the object `uri`. Throws ApiError, a NotFound one when
  // the object does not exist.
  GcsObjectMetadata Metadata(const std::string& uri);

  // Throws ApiError.
  void Download(const std::string& uri, const std::filesystem::path& output);

  // Writes the file `input` to the object `uri`, replacing it. Throws ApiError.
  void Upload(const std::filesystem::path& input, const std::string& uri);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bigquery_emulator_duckdb
