#pragma once

#include <filesystem>
#include <string>

namespace bigquery_emulator_duckdb {

// Downloads the object named by a gs://bucket/object URI into `output`. It reads from
// STORAGE_EMULATOR_HOST when set, such as fake-gcs-server, and from the public Storage API
// otherwise, sending GOOGLE_OAUTH_ACCESS_TOKEN as a bearer token when set. Throws ApiError.
void DownloadGcsObject(const std::string& uri, const std::filesystem::path& output);

}  // namespace bigquery_emulator_duckdb
