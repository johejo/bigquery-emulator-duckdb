#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace bigquery_emulator_duckdb {

// Retains SDK connection pools and credentials for the lifetime of the emulator. Concurrent
// downloads are supported.
class GcsClient {
 public:
  // Reads the endpoint from the environment: CLOUD_STORAGE_EMULATOR_ENDPOINT or
  // CLOUD_STORAGE_TESTBENCH_ENDPOINT, which the SDK itself honors, then STORAGE_EMULATOR_HOST,
  // and otherwise the public Storage API. Any of the variables selects an emulator.
  GcsClient();
  // Emulators are always read anonymously. Other endpoints read public objects anonymously
  // and private objects with Application Default Credentials.
  GcsClient(std::string endpoint, bool emulator);
  ~GcsClient();

  // The Storage API endpoint downloads read from.
  const std::string& endpoint() const;

  // Throws ApiError.
  void Download(const std::string& uri, const std::filesystem::path& output);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bigquery_emulator_duckdb
