#include "src/gcs.h"

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "httplib.h"
#include "src/api_error.h"

namespace bigquery_emulator_duckdb {
namespace {

constexpr char kUri[] = "gs://bucket/nested/a.json";

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), {}};
}

class GcsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (const char* name :
         {"STORAGE_EMULATOR_HOST", "GOOGLE_APPLICATION_CREDENTIALS",
          "CLOUD_STORAGE_EMULATOR_ENDPOINT", "CLOUD_STORAGE_TESTBENCH_ENDPOINT"}) {
      const char* value = std::getenv(name);
      environment_[name] = value != nullptr ? std::optional<std::string>(value) : std::nullopt;
      unsetenv(name);
    }
    std::string pattern = ::testing::TempDir() + "/gcs-test-XXXXXX";
    const char* directory = mkdtemp(pattern.data());
    ASSERT_NE(directory, nullptr);
    directory_ = directory;
    output_ = directory_ / "object";
    // Deliberately invalid ADCs ensure emulators and public reads never try to load them.
    SetEnv("GOOGLE_APPLICATION_CREDENTIALS", (directory_ / "missing-credentials.json").string());
    server_.Get(".*", [this](const httplib::Request& request, httplib::Response& response) {
      if (requests_++ < transient_failures_) {
        response.status = 503;
        return;
      }
      if (require_adc_ && request.get_header_value("Authorization").empty()) {
        response.status = 401;
        return;
      }
      if (check_connection_reuse_) {
        const int previous_port = peer_port_.exchange(request.remote_port);
        if (previous_port != 0) EXPECT_EQ(request.remote_port, previous_port);
      }
      EXPECT_EQ(request.get_header_value("Authorization"), authorization_);
      if (list_objects_) {
        EXPECT_EQ(request.path, "/storage/v1/b/bucket/o");
        EXPECT_EQ(request.get_param_value("prefix"), "nested/a");
        if (request.get_param_value("pageToken").empty()) {
          response.set_content(R"({"items":[{"name":"nested/a2.json"}],"nextPageToken":"next"})",
                               "application/json");
        } else {
          EXPECT_EQ(request.get_param_value("pageToken"), "next");
          response.set_content(R"({"items":[{"name":"nested/a1.json"}]})", "application/json");
        }
        return;
      }
      EXPECT_EQ(request.path, expected_path_);
      EXPECT_EQ(request.get_param_value("alt"), "media");
      response.set_content("fixture\n", "application/octet-stream");
    });
    server_.Post("/token", [this](const httplib::Request&, httplib::Response& response) {
      ++token_requests_;
      response.set_content(
          R"({"access_token":"adc-token","expires_in":3600,"token_type":"Bearer",)"
          R"("issued_token_type":"urn:ietf:params:oauth:token-type:access_token"})",
          "application/json");
    });
    const int port = server_.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    endpoint_ = "http://127.0.0.1:" + std::to_string(port);
    SetEnv("STORAGE_EMULATOR_HOST", endpoint_);
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }

  void TearDown() override {
    server_.stop();
    if (thread_.joinable()) thread_.join();
    if (!directory_.empty()) std::filesystem::remove_all(directory_);
    for (const auto& [name, value] : environment_) {
      if (value) {
        SetEnv(name, *value);
      } else {
        unsetenv(name.c_str());
      }
    }
  }

  static void SetEnv(const std::string& name, const std::string& value) {
    ASSERT_EQ(setenv(name.c_str(), value.c_str(), 1), 0);
  }

  // The client reads the environment on construction, so it is created on first use after a
  // test has configured the environment.
  GcsClient& client() {
    if (!client_) client_.emplace();
    return *client_;
  }

  // Points ADC at credentials that exchange a token with the test server.
  void WriteAdc() {
    const auto subject = directory_ / "subject-token";
    std::ofstream(subject) << "subject-token";
    const auto credentials = directory_ / "credentials.json";
    std::ofstream(credentials)
        << R"({"type":"external_account",)"
        << R"("audience":"//iam.googleapis.com/projects/123/locations/global/workloadIdentityPools/pool/providers/provider",)"
        << R"("subject_token_type":"urn:ietf:params:oauth:token-type:jwt",)"
        << R"("token_url":")" << endpoint_ << R"(/token",)"
        << R"("credential_source":{"file":")" << subject.string() << R"("}})";
    SetEnv("GOOGLE_APPLICATION_CREDENTIALS", credentials.string());
  }

  void Download() {
    client().Download(kUri, output_);
    EXPECT_EQ(ReadFile(output_), "fixture\n");
  }

  std::optional<GcsClient> client_;
  httplib::Server server_;
  std::thread thread_;
  std::map<std::string, std::optional<std::string>> environment_;
  std::filesystem::path directory_;
  std::filesystem::path output_;
  std::string endpoint_;
  std::string authorization_;
  std::string expected_path_ = "/storage/v1/b/bucket/o/nested/a.json";
  bool list_objects_ = false;
  bool require_adc_ = false;
  bool check_connection_reuse_ = false;
  std::atomic<int> peer_port_ = 0;
  std::atomic<int> requests_ = 0;
  std::atomic<int> token_requests_ = 0;
  int transient_failures_ = 0;
};

TEST_F(GcsTest, ReusesConnections) {
  check_connection_reuse_ = true;
  Download();
  Download();
  EXPECT_EQ(requests_, 2);
}

TEST_F(GcsTest, AcceptsPathPrefix) {
  SetEnv("STORAGE_EMULATOR_HOST", endpoint_ + "/prefix///");
  expected_path_ = "/prefix" + expected_path_;
  EXPECT_EQ(client().endpoint(), endpoint_ + "/prefix");
  Download();
}

TEST_F(GcsTest, DefaultsToPublicStorageApi) {
  unsetenv("STORAGE_EMULATOR_HOST");
  EXPECT_EQ(client().endpoint(), "https://storage.googleapis.com");
}

TEST_F(GcsTest, PrefersSdkEmulatorVariables) {
  SetEnv("STORAGE_EMULATOR_HOST", "http://127.0.0.1:1");
  SetEnv("CLOUD_STORAGE_EMULATOR_ENDPOINT", endpoint_);
  EXPECT_EQ(client().endpoint(), endpoint_);
  Download();
}

TEST_F(GcsTest, NeverUsesAdcWithEmulator) {
  for (const char* name : {"STORAGE_EMULATOR_HOST", "CLOUD_STORAGE_EMULATOR_ENDPOINT"}) {
    SCOPED_TRACE(name);
    unsetenv("STORAGE_EMULATOR_HOST");
    SetEnv(name, endpoint_);
    WriteAdc();
    require_adc_ = true;
    client_.reset();
    EXPECT_THROW(client().Download(kUri, output_), ApiError);
    EXPECT_EQ(token_requests_, 0);
    unsetenv(name);
  }
}

TEST_F(GcsTest, ReadsAnonymouslyWithoutAdc) {
  client_.emplace(endpoint_, false);
  Download();
  require_adc_ = true;
  EXPECT_THROW(client().Download(kUri, output_), ApiError);
}

TEST_F(GcsTest, UsesAdcWhenAvailable) {
  client_.emplace(endpoint_, false);
  WriteAdc();
  authorization_ = "Bearer adc-token";
  Download();
  Download();
  // The availability check and the Storage client each exchange a token once.
  EXPECT_LE(token_requests_, 2);
  EXPECT_EQ(requests_, 2);
}

TEST_F(GcsTest, IgnoresEnvironmentChangesAfterConstruction) {
  client();
  SetEnv("STORAGE_EMULATOR_HOST", "http://127.0.0.1:1");
  Download();
}

TEST_F(GcsTest, SupportsConcurrentDownloads) {
  GcsClient& client = this->client();
  std::vector<std::thread> downloads;
  downloads.reserve(4);
  for (int i = 0; i < 4; ++i) {
    downloads.emplace_back([this, &client, i] {
      EXPECT_NO_THROW(client.Download(kUri, directory_ / std::to_string(i)));
    });
  }
  for (auto& download : downloads) download.join();
  for (int i = 0; i < 4; ++i) EXPECT_EQ(ReadFile(directory_ / std::to_string(i)), "fixture\n");
}

TEST_F(GcsTest, RetriesTransientFailures) {
  transient_failures_ = 1;
  Download();
}

TEST_F(GcsTest, ExactUriDoesNotListObjects) {
  EXPECT_EQ(client().Expand(kUri), std::vector<std::string>({kUri}));
  EXPECT_EQ(requests_, 0);
}

TEST_F(GcsTest, ListsAllPages) {
  list_objects_ = true;
  EXPECT_EQ(client().Expand("gs://bucket/nested/a*.json"),
            std::vector<std::string>({"gs://bucket/nested/a1.json", "gs://bucket/nested/a2.json"}));
  EXPECT_EQ(requests_, 2);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
