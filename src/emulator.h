#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_initializer.h"
#include "src/builtin_function_options.h"
#include "src/external_query.h"
#include "src/function_options.h"
#include "src/gcs.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_geography/options.h"
#include "src/location.h"
#include "src/project.h"
#include "src/references.h"
#include "src/sql_runtime.h"  // IWYU pragma: export

namespace bigquery_emulator_duckdb {

struct LoadJob {
  TableReference destination_table;
  CreateDisposition create_disposition = CreateDisposition::kCreateIfNeeded;
  WriteDisposition write_disposition = WriteDisposition::kWriteAppend;
  // Wire configuration is decoded during execution so errors remain recorded job failures.
  nlohmann::json configuration;
};

// The enumerators take BigQuery's names.
enum class CopyOperation : std::uint8_t { kCopy, kSnapshot, kRestore, kClone };

struct CopyJob {
  CopyOperation operation = CopyOperation::kCopy;
  std::vector<TableReference> source_tables;
  TableReference destination_table;
  CreateDisposition create_disposition = CreateDisposition::kCreateIfNeeded;
  WriteDisposition write_disposition = WriteDisposition::kWriteEmpty;
  // destinationExpirationTime, in milliseconds since the Unix epoch.
  std::optional<int64_t> destination_expiration_time;
  nlohmann::json configuration;
};

struct ExtractJob {
  TableReference source_table;
  std::vector<std::string> destination_uris;
  nlohmann::json configuration;
};

struct Job {
  std::string project_id;
  std::string job_id;
  std::string location = kDefaultLocation;
  std::variant<QueryJob, LoadJob, CopyJob, ExtractJob> configuration;
  int64_t output_rows = 0;
  int64_t creation_time_ms = 0;
  int64_t end_time_ms = 0;
  // Exactly one is set when the synchronous job completes.
  std::optional<QueryResult> result;
  std::optional<ApiError> error;

  [[nodiscard]] const QueryJob* query() const { return std::get_if<QueryJob>(&configuration); }
  [[nodiscard]] bool dry_run() const { return query() != nullptr && query()->dry_run; }
};

struct LoadRequest {
  std::string project_id;
  std::optional<std::string> location;
  std::string job_id;  // Generated when empty.
  LoadJob load;
};

struct CopyRequest {
  std::string project_id;
  std::optional<std::string> location;
  std::string job_id;  // Generated when empty.
  CopyJob copy;
};

struct ExtractRequest {
  std::string project_id;
  std::optional<std::string> location;
  std::string job_id;  // Generated when empty.
  ExtractJob extract;
};

// Application facade: native catalog APIs delegate to the shared SQL runtime, while this
// class owns job bookkeeping and external services. Transports need no runtime internals.
class Emulator : public SqlRuntime {
 public:
  explicit Emulator(
      std::string data_dir = "", const std::vector<Project>& projects = {},
      const std::optional<std::string>& session_user = std::nullopt,
      const BackendInitializer& initialize = {},
      const std::vector<ExternalConnection>& connections = std::vector<ExternalConnection>(),
      const googlesql_geography::Options& geography = {.expand_bounding_box = true},
      const functions_duckdb::Options& functions = BigQueryFunctionOptions(),
      ExcludedFunctionIds excluded_function_ids = BigQueryExcludedFunctionIds(),
      FunctionDeclarations declarations = FunctionDeclarations::kBigQuery);

  const std::string& storage_endpoint() const { return gcs_client_.endpoint(); }
  // Query errors are recorded in jobs rather than thrown by the facade.
  std::shared_ptr<const Job> RunQuery(QueryRequest request);
  std::shared_ptr<const Job> RunLoad(LoadRequest request);
  std::shared_ptr<const Job> RunCopy(CopyRequest request);
  std::shared_ptr<const Job> RunExtract(ExtractRequest request);
  std::shared_ptr<const Job> GetJob(std::string project_id, const std::string& job_id,
                                    const std::optional<std::string>& location = std::nullopt);
  std::vector<std::shared_ptr<const Job>> ListJobs(std::string project_id);
  void DeleteJob(std::string project_id, const std::string& job_id,
                 const std::optional<std::string>& location = std::nullopt);

 private:
  std::shared_ptr<const Job> RunJob(std::shared_ptr<Job> job, JobLocation& location,
                                    const std::function<void(Job&)>& body);

  ExternalConnections connections_{.backend = backend_};
  GcsClient gcs_client_;
  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<const Job>> jobs_;
  std::unordered_set<std::string> running_jobs_;
  int64_t next_job_number_ = 1;
};

}  // namespace bigquery_emulator_duckdb
