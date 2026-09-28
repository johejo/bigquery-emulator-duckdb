#include "src/gcs.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "google/cloud/credentials.h"
#include "google/cloud/options.h"
#include "google/cloud/status.h"
#include "google/cloud/storage/client.h"
#include "google/cloud/storage/options.h"
#include "src/api_error.h"

namespace bigquery_emulator_duckdb {

namespace cloud = google::cloud;
namespace storage = cloud::storage;

namespace {

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
  // Emulators are always read anonymously. Otherwise public objects are read anonymously
  // first and ADC is loaded only when the Storage API rejects the request, so hosts without
  // ADC can still read public objects.
  Impl(std::string endpoint, bool emulator)
      : options(cloud::Options{}
                    .set<storage::RestEndpointOption>(std::move(endpoint))
                    .set<cloud::UnifiedCredentialsOption>(cloud::MakeInsecureCredentials())),
        use_adc(!emulator),
        client(options) {}

  cloud::Options options;
  bool use_adc;
  storage::Client client;
  std::once_flag initialize_adc;
  std::optional<storage::Client> adc;
  // Set once a download has needed ADC, so later downloads skip the anonymous attempt.
  std::atomic<bool> prefer_adc = false;
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

void GcsClient::Download(const std::string& uri, const std::filesystem::path& output) {
  const size_t slash = uri.find('/', 5);
  if (!uri.starts_with("gs://") || slash == std::string::npos || slash == 5 ||
      slash + 1 == uri.size()) {
    throw ApiError::Invalid("Invalid GCS URI: " + uri);
  }
  const std::string bucket = uri.substr(5, slash - 5);
  const std::string object = uri.substr(slash + 1);
  // SDK copies share the connection pool. Concurrent calls on the same Client instance
  // are not guaranteed to work, so each download uses its own lightweight copy.
  auto download = [&](storage::Client client) {
    return client.DownloadToFile(bucket, object, output.string());
  };
  auto download_with_adc = [&] {
    std::call_once(impl_->initialize_adc, [this] {
      auto options = impl_->options;
      options.set<cloud::UnifiedCredentialsOption>(cloud::MakeGoogleDefaultCredentials());
      impl_->adc.emplace(options);
    });
    return download(*impl_->adc);
  };
  cloud::Status status;
  if (impl_->prefer_adc) {
    status = download_with_adc();
  } else {
    status = download(impl_->client);
    if (impl_->use_adc && (status.code() == cloud::StatusCode::kUnauthenticated ||
                           status.code() == cloud::StatusCode::kPermissionDenied)) {
      status = download_with_adc();
      if (status.ok()) impl_->prefer_adc = true;
    }
  }
  if (!status.ok()) {
    throw ApiError::Invalid("Could not read GCS object: " + uri + " (" + status.message() + ")");
  }
}

}  // namespace bigquery_emulator_duckdb
