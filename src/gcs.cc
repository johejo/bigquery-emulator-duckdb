#include "src/gcs.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "google/cloud/credentials.h"
#include "google/cloud/oauth2/access_token_generator.h"
#include "google/cloud/options.h"
#include "google/cloud/status.h"
#include "google/cloud/storage/client.h"
#include "google/cloud/storage/options.h"
#include "src/api_error.h"

namespace bigquery_emulator_duckdb {

namespace cloud = google::cloud;
namespace storage = cloud::storage;

namespace {

struct ObjectUri {
  std::string bucket;
  std::string object;
};

ObjectUri ParseUri(const std::string& uri) {
  const size_t slash = uri.find('/', 5);
  if (!uri.starts_with("gs://") || slash == std::string::npos || slash == 5 ||
      slash + 1 == uri.size()) {
    throw ApiError::Invalid("Invalid GCS URI: " + uri);
  }
  if (uri.substr(5, slash - 5).find('*') != std::string::npos) {
    throw ApiError::Invalid("Wildcard is not allowed in a bucket name: " + uri);
  }
  return {uri.substr(5, slash - 5), uri.substr(slash + 1)};
}

struct Endpoint {
  std::string url;
  bool emulator;
};

Endpoint EndpointFromEnvironment() {
  // The SDK reads these itself and lets them override any configured endpoint, so they take
  // precedence here too.
  for (const char* name : {"CLOUD_STORAGE_EMULATOR_ENDPOINT", "CLOUD_STORAGE_TESTBENCH_ENDPOINT"}) {
    if (const char* value = std::getenv(name)) return {value, true};
  }
  const char* host = std::getenv("STORAGE_EMULATOR_HOST");
  if (host == nullptr || *host == '\0') return {"https://storage.googleapis.com", false};
  std::string url = host;
  while (!url.empty() && url.back() == '/') url.pop_back();
  // Like the Google client libraries, accept a bare "host:port" as an HTTP endpoint.
  if (url.find("://") == std::string::npos) url = "http://" + url;
  return {url, true};
}

}  // namespace

struct GcsClient::Impl {
  Impl(std::string endpoint, bool emulator)
      : options(cloud::Options{}
                    .set<storage::RestEndpointOption>(std::move(endpoint))
                    .set<cloud::UnifiedCredentialsOption>(cloud::MakeInsecureCredentials())),
        emulator(emulator),
        anonymous(options) {}

  // Emulators are always read anonymously. Otherwise ADC is used when it yields a token and
  // anonymous access otherwise. Without a credentials file ADC falls back to the metadata
  // server, so the check waits for the first download rather than delaying startup.
  storage::Client& client() {
    if (emulator) return anonymous;
    std::call_once(select_credentials, [this] {
      auto credentials = cloud::MakeGoogleDefaultCredentials();
      if (!cloud::oauth2::MakeAccessTokenGenerator(*credentials)->GetToken()) return;
      adc.emplace(
          cloud::Options(options).set<cloud::UnifiedCredentialsOption>(std::move(credentials)));
    });
    return adc ? *adc : anonymous;
  }

  cloud::Options options;
  bool emulator;
  storage::Client anonymous;
  std::once_flag select_credentials;
  std::optional<storage::Client> adc;
};

GcsClient::GcsClient() {
  Endpoint endpoint = EndpointFromEnvironment();
  impl_ = std::make_unique<Impl>(std::move(endpoint.url), endpoint.emulator);
}

GcsClient::GcsClient(std::string endpoint, bool emulator)
    : impl_(std::make_unique<Impl>(std::move(endpoint), emulator)) {}

GcsClient::~GcsClient() = default;

const std::string& GcsClient::endpoint() const {
  return impl_->options.get<storage::RestEndpointOption>();
}

std::vector<std::string> GcsClient::Expand(const std::string& uri) {
  const auto [bucket, object] = ParseUri(uri);
  const size_t wildcard = object.find('*');
  if (wildcard == std::string::npos) return {uri};
  if (object.find('*', wildcard + 1) != std::string::npos) {
    throw ApiError::Invalid("Only one wildcard is allowed in a GCS URI: " + uri);
  }
  const std::string prefix = object.substr(0, wildcard);
  const std::string suffix = object.substr(wildcard + 1);
  // The documented file-name pattern (fed-sample*.csv) excludes subfolders,
  // whereas *.csv and a trailing prefix wildcard include them.
  // https://cloud.google.com/bigquery/docs/batch-loading-data#load-wildcards
  const bool filename_pattern = !prefix.empty() && prefix.back() != '/' && !suffix.empty() &&
                                suffix.find('/') == std::string::npos;
  storage::Client client = impl_->client();
  const std::string bucket_uri = "gs://" + bucket + "/";
  std::vector<std::string> matches;
  for (const auto& entry : client.ListObjects(bucket, storage::Prefix(prefix))) {
    if (!entry) {
      throw ApiError::Invalid("Could not list GCS objects: " + uri + " (" +
                              entry.status().message() + ")");
    }
    const std::string& name = entry->name();
    if (name.size() >= prefix.size() + suffix.size() && name.starts_with(prefix) &&
        name.ends_with(suffix)) {
      if (filename_pattern && name.find('/', prefix.size()) != std::string::npos) continue;
      matches.push_back(bucket_uri + name);
    }
  }
  if (matches.empty()) throw ApiError::Invalid("No GCS objects match: " + uri);
  std::sort(matches.begin(), matches.end());
  return matches;
}

void GcsClient::Download(const std::string& uri, const std::filesystem::path& output) {
  const auto [bucket, object] = ParseUri(uri);
  // SDK copies share the connection pool. Concurrent calls on the same Client instance
  // are not guaranteed to work, so each download uses its own lightweight copy.
  storage::Client client = impl_->client();
  const cloud::Status status = client.DownloadToFile(bucket, object, output.string());
  if (!status.ok()) {
    throw ApiError::Invalid("Could not read GCS object: " + uri + " (" + status.message() + ")");
  }
}

}  // namespace bigquery_emulator_duckdb
