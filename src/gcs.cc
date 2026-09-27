#include "src/gcs.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

#include "httplib.h"
#include "src/api_error.h"

namespace bigquery_emulator_duckdb {
namespace {

// Encodes a path segment. The Storage JSON API takes an object name as a single segment, so
// the slashes in it must be encoded too.
std::string UrlEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += kHex[c >> 4];
      encoded += kHex[c & 15];
    }
  }
  return encoded;
}

}  // namespace

void DownloadGcsObject(const std::string& uri, const std::filesystem::path& output) {
  const size_t slash = uri.find('/', 5);
  if (slash == std::string::npos || slash == 5 || slash + 1 == uri.size()) {
    throw ApiError::Invalid("Invalid GCS URI: " + uri);
  }
  const std::string bucket = uri.substr(5, slash - 5);
  const std::string object = uri.substr(slash + 1);
  const char* emulator_host = std::getenv("STORAGE_EMULATOR_HOST");
  std::string base =
      emulator_host && *emulator_host ? emulator_host : "https://storage.googleapis.com";
  while (!base.empty() && base.back() == '/') base.pop_back();
  // Like the Google client libraries, accept a bare "host:port" as an HTTP endpoint.
  if (base.find("://") == std::string::npos) base = "http://" + base;
  const size_t path_start = base.find('/', base.find("://") + 3);
  const std::string origin = base.substr(0, path_start);
  const std::string path_prefix = path_start == std::string::npos ? "" : base.substr(path_start);
  httplib::Client client(origin);
  if (!client.is_valid()) throw ApiError::Internal("Invalid Storage endpoint: " + base);
  client.set_follow_location(true);
  const char* token = std::getenv("GOOGLE_OAUTH_ACCESS_TOKEN");
  if (token && *token) client.set_bearer_token_auth(token);
  // The body is streamed straight into the file rather than held in memory.
  std::ofstream file(output, std::ios::binary | std::ios::trunc);
  if (!file) throw ApiError::Internal("Could not open load temporary file");
  int status = 0;
  const httplib::Result result = client.Get(
      path_prefix + "/storage/v1/b/" + UrlEncode(bucket) + "/o/" + UrlEncode(object) + "?alt=media",
      [&status](const httplib::Response& response) {
        status = response.status;
        return status >= 200 && status < 300;
      },
      [&file](const char* data, size_t length) {
        file.write(data, static_cast<std::streamsize>(length));
        return file.good();
      });
  file.close();
  if (status != 0 && (status < 200 || status >= 300)) {
    throw ApiError::Invalid("Could not read GCS object: " + uri + " (HTTP " +
                            std::to_string(status) + ")");
  }
  if (!result) {
    throw ApiError::Invalid("Could not read GCS object: " + uri + " (" +
                            httplib::to_string(result.error()) + ")");
  }
  if (!file) throw ApiError::Internal("Could not write load temporary file");
}

}  // namespace bigquery_emulator_duckdb
