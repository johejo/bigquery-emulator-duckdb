#include "src/gcs.h"

// include-cleaner requires stdlib.h for the POSIX mkdtemp/setenv/unsetenv APIs;
// modernize-deprecated-headers suggests cstdlib instead, so suppress it on this include.
// On macOS, include-cleaner still misses these declarations through libc++'s stdlib.h wrapper;
// suppress those false positives at the calls.
#include <stdlib.h>  // NOLINT(modernize-deprecated-headers)

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
#include "src/api_error.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/file_read_options.h"
#include "src/httplib.h"
#include "src/temporary_files.h"

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
    for (const char* name : {
             "STORAGE_EMULATOR_HOST",
             "GOOGLE_APPLICATION_CREDENTIALS",
             "CLOUD_STORAGE_EMULATOR_ENDPOINT",
             "CLOUD_STORAGE_TESTBENCH_ENDPOINT",
         }) {
      const char* value = std::getenv(name);
      environment_[name] = value != nullptr ? std::optional<std::string>(value) : std::nullopt;
      unsetenv(name);  // NOLINT(misc-include-cleaner)
    }
    std::string pattern = ::testing::TempDir() + "/gcs-test-XXXXXX";
    const char* directory = mkdtemp(pattern.data());  // NOLINT(misc-include-cleaner)
    ASSERT_NE(directory, nullptr);
    directory_ = directory;
    output_ = directory_ / "object";
    // Deliberately invalid ADCs ensure emulators and public reads never try to load them.
    SetEnv("GOOGLE_APPLICATION_CREDENTIALS", (directory_ / "missing-credentials.json").string());
    server_.Get(".*", [this](const httplib::Request& request, httplib::Response& response) {
      ++requests_;
      if (require_adc_ && request.get_header_value("Authorization").empty()) {
        response.status = 401;
        return;
      }
      EXPECT_EQ(request.get_header_value("Authorization"), authorization_);
      EXPECT_EQ(request.path, "/storage/v1/b/bucket/o/nested/a.json");
      EXPECT_EQ(request.get_param_value("alt"), "media");
      response.set_content(contents_, "application/octet-stream");
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
    if (thread_.joinable()) {
      thread_.join();
    }
    if (!directory_.empty()) {
      std::filesystem::remove_all(directory_);
    }
    for (const auto& [name, value] : environment_) {
      if (value) {
        SetEnv(name, *value);
      } else {
        unsetenv(name.c_str());
      }
    }
  }

  static void SetEnv(const std::string& name, const std::string& value) {
    ASSERT_EQ(setenv(name.c_str(), value.c_str(), 1), 0);  // NOLINT(misc-include-cleaner)
  }

  // The client reads the environment on construction, so it is created on first use after a
  // test has configured the environment.
  GcsClient& client() {
    if (!client_) {
      client_.emplace();
    }
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
  std::string contents_ = "fixture\n";
  std::string endpoint_;
  std::string authorization_;
  bool require_adc_ = false;
  std::atomic<int> requests_ = 0;
  std::atomic<int> token_requests_ = 0;
};

TEST_F(GcsTest, StagesSourcesWithCallerOwnedLifetime) {
  std::string path;
  {
    TemporaryFiles files;
    const auto sources = StageFileSources({kUri}, FileFormat::kJson, client(), files);
    ASSERT_EQ(sources.size(), 1);
    EXPECT_EQ(sources.front().uri, kUri);
    path = sources.front().path;
    EXPECT_EQ(ReadFile(path), "fixture\n");
    EXPECT_EQ(requests_, 1);
  }
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(GcsTest, JsonStagingKeepsSourceIdentityAndCleansUp) {
  contents_ = R"({"value":1.234567890123456789})";
  std::string downloaded;
  std::string rewritten;
  {
    TemporaryFiles files;
    const auto sources = StageFileSources({kUri}, FileFormat::kJson, client(), files);
    const auto staged =
        StageJsonNumerics(sources, {{.name = "value", .type = FieldType::kBigNumeric}}, files);
    ASSERT_EQ(staged.size(), 1);
    EXPECT_EQ(staged.front().uri, kUri);
    downloaded = sources.front().path;
    rewritten = staged.front().path;
    EXPECT_NE(downloaded, rewritten);
    EXPECT_TRUE(std::filesystem::exists(downloaded));
    EXPECT_TRUE(std::filesystem::exists(rewritten));
  }
  EXPECT_FALSE(std::filesystem::exists(downloaded));
  EXPECT_FALSE(std::filesystem::exists(rewritten));
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

TEST_F(GcsTest, SupportsConcurrentDownloads) {
  GcsClient& client = this->client();
  std::vector<std::thread> downloads;
  downloads.reserve(4);
  for (int i = 0; i < 4; ++i) {
    downloads.emplace_back([this, &client, i] {
      EXPECT_NO_THROW(client.Download(kUri, directory_ / std::to_string(i)));
    });
  }
  for (auto& download : downloads) {
    download.join();
  }
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(ReadFile(directory_ / std::to_string(i)), "fixture\n");
  }
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
