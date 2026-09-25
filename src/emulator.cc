#include "src/emulator.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

#include "absl/status/statusor.h"
#include "googlesql/public/type.h"
#include "nlohmann/json.hpp"
#include "src/analyzer.h"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The file a project is stored in. Project ids may carry a domain ("example.com:project"), so
// every byte outside [A-Za-z0-9_-] is percent-encoded. With '.' and '/' encoded, no id can name a
// path outside the data directory.
std::string ProjectFileName(const std::string& project_id) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string name;
  for (const char c : project_id) {
    const auto byte = static_cast<unsigned char>(c);
    if (std::isalnum(byte) != 0 || c == '_' || c == '-') {
      name += c;
    } else {
      name += '%';
      name += kHex[byte >> 4];
      name += kHex[byte & 0xF];
    }
  }
  return name + ".duckdb";
}

std::string QualifiedName(const DatasetReference& dataset) {
  return QuoteIdentifier(dataset.project_id) + "." + QuoteIdentifier(dataset.dataset_id);
}

std::string QualifiedName(const TableReference& table) {
  return QuoteIdentifier(table.project_id) + "." + QuoteIdentifier(table.dataset_id) + "." +
         QuoteIdentifier(table.table_id);
}

std::string JobKey(const std::string& project_id, const std::string& job_id) {
  return project_id + ":" + job_id;
}

std::string UrlEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += kHex[c >> 4];
      encoded += kHex[c & 15];
    }
  }
  return encoded;
}

// curl handles HTTPS and the standard STORAGE_EMULATOR_HOST HTTP endpoint. Arguments are passed
// directly to exec, so object names and credentials never pass through a shell.
void DownloadGcs(const std::string& uri, const std::string& output) {
  const size_t slash = uri.find('/', 5);
  if (slash == std::string::npos || slash == 5 || slash + 1 == uri.size()) {
    throw ApiError::Invalid("Invalid GCS URI: " + uri);
  }
  const std::string bucket = uri.substr(5, slash - 5);
  const std::string object = uri.substr(slash + 1);
  const char* emulator_host = std::getenv("STORAGE_EMULATOR_HOST");
  std::string base = emulator_host && *emulator_host ? emulator_host : "https://storage.googleapis.com";
  while (!base.empty() && base.back() == '/') base.pop_back();
  const std::string url = base + "/storage/v1/b/" + UrlEncode(bucket) + "/o/" +
                          UrlEncode(object) + "?alt=media";
  const char* token = std::getenv("GOOGLE_OAUTH_ACCESS_TOKEN");
  std::string authorization = token && *token ? std::string("Authorization: Bearer ") + token : "";
  std::vector<const char*> argv = {"curl", "--fail", "--silent", "--show-error", "--location",
                                   "--output", output.c_str()};
  if (!authorization.empty()) {
    argv.push_back("--header");
    argv.push_back(authorization.c_str());
  }
  argv.push_back(url.c_str());
  argv.push_back(nullptr);
  const pid_t pid = fork();
  if (pid < 0) throw ApiError::Internal("Could not start GCS download");
  if (pid == 0) {
    execvp("curl", const_cast<char* const*>(argv.data()));
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw ApiError::Invalid("Could not read GCS object: " + uri);
  }
}

struct DownloadedFiles {
  std::vector<std::string> paths;
  ~DownloadedFiles() {
    for (const std::string& path : paths) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
};

FieldSchema ParseField(const json& value) {
  FieldSchema field{value.at("name").get<std::string>(), value.value("type", "STRING"),
                    value.value("mode", "NULLABLE"), {}};
  for (const json& child : value.value("fields", json::array())) {
    field.fields.push_back(ParseField(child));
  }
  return field;
}

std::vector<std::string> FirstColumnStrings(const QueryResult& result) {
  std::vector<std::string> values;
  values.reserve(result.rows.size());
  for (const json& row : result.rows) {
    values.push_back(row["f"][0]["v"].get<std::string>());
  }
  return values;
}

// Maps a BigQuery TableFieldSchema to a DuckDB column type.
std::string ToDuckDbType(const json& field) {
  const std::string type = field.value("type", "STRING");
  std::string duckdb_type;
  if (type == "INTEGER" || type == "INT64") {
    duckdb_type = "BIGINT";
  } else if (type == "FLOAT" || type == "FLOAT64") {
    duckdb_type = "DOUBLE";
  } else if (type == "BOOLEAN" || type == "BOOL") {
    duckdb_type = "BOOLEAN";
  } else if (type == "STRING" || type == "GEOGRAPHY") {
    duckdb_type = "VARCHAR";
  } else if (type == "BYTES") {
    duckdb_type = "BLOB";
  } else if (type == "DATE" || type == "TIME" || type == "JSON") {
    duckdb_type = type;
  } else if (type == "DATETIME") {
    duckdb_type = "TIMESTAMP";
  } else if (type == "TIMESTAMP") {
    duckdb_type = "TIMESTAMPTZ";
  } else if (type == "NUMERIC") {
    duckdb_type = "DECIMAL(38, 9)";
  } else if (type == "BIGNUMERIC") {
    duckdb_type = "DECIMAL(38, 19)";
  } else if (type == "RECORD" || type == "STRUCT") {
    duckdb_type = "STRUCT(";
    bool first = true;
    for (const json& child : field.value("fields", json::array())) {
      if (!first) {
        duckdb_type += ", ";
      }
      first = false;
      duckdb_type +=
          QuoteIdentifier(child.at("name").get<std::string>()) + " " + ToDuckDbType(child);
    }
    duckdb_type += ")";
  } else {
    throw ApiError::Invalid("Unsupported field type: " + type);
  }
  if (field.value("mode", "NULLABLE") == "REPEATED") {
    duckdb_type += "[]";
  }
  return duckdb_type;
}

std::string InsertValue(const json& value, const FieldSchema& field, bool ignore_unknown_values);

std::string InsertRecord(const json& value, const FieldSchema& field, bool ignore_unknown_values) {
  if (!value.is_object()) {
    throw ApiError::Invalid("Expected an object for field " + field.name);
  }
  for (auto it = value.begin(); it != value.end(); ++it) {
    const bool known =
        std::any_of(field.fields.begin(), field.fields.end(),
                    [&](const FieldSchema& child) { return child.name == it.key(); });
    if (!known && !ignore_unknown_values) {
      throw ApiError::Invalid("Unknown field: " + it.key());
    }
  }
  std::string fields;
  for (const FieldSchema& child : field.fields) {
    if (!fields.empty()) {
      fields += ", ";
    }
    const auto it = value.find(child.name);
    fields += QuoteIdentifier(child.name) + " := " +
              InsertValue(it == value.end() ? json(nullptr) : *it, child, ignore_unknown_values);
  }
  return "struct_pack(" + fields + ")";
}

std::string InsertValue(const json& value, const FieldSchema& field, bool ignore_unknown_values) {
  const std::string type = ToDuckDbType(field.ToJson());
  if (value.is_null()) {
    return "CAST(NULL AS " + type + ")";
  }
  if (field.mode == "REPEATED") {
    if (!value.is_array()) {
      throw ApiError::Invalid("Expected an array for field " + field.name);
    }
    FieldSchema element = field;
    element.mode = "NULLABLE";
    std::string values;
    for (const json& item : value) {
      if (!values.empty()) {
        values += ", ";
      }
      values += InsertValue(item, element, ignore_unknown_values);
    }
    return "CAST([" + values + "] AS " + type + ")";
  }
  if (field.type == "RECORD") {
    return InsertRecord(value, field, ignore_unknown_values);
  }
  if (!value.is_primitive()) {
    throw ApiError::Invalid("Expected a scalar for field " + field.name);
  }
  const std::string scalar = value.is_string() ? value.get<std::string>() : value.dump();
  if (field.type == "BYTES") {
    return "from_base64(" + QuoteLiteral(scalar) + ")";
  }
  if (field.type == "TIMESTAMP" && value.is_number()) {
    return "to_timestamp(CAST(" + QuoteLiteral(scalar) + " AS DOUBLE))";
  }
  return "CAST(" + QuoteLiteral(scalar) + " AS " + type + ")";
}

// Serves the analyzer the tables the emulator keeps in DuckDB.
class DuckDbTableSource : public TableSource {
 public:
  explicit DuckDbTableSource(Backend& backend) : backend_(backend) {}

  std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                    const std::string& dataset,
                                                    const std::string& table) override {
    try {
      return backend_
          .Prepare("SELECT * FROM " + QualifiedName(TableReference{project, dataset, table}))
          .schema;
    } catch (const BackendError&) {
      return std::nullopt;
    }
  }

 private:
  Backend& backend_;
};

const googlesql::Type* ParameterType(const FieldSchema& field,
                                     googlesql::TypeFactory& type_factory) {
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
  if (!type.ok()) {
    throw ApiError::InvalidQuery(std::string(type.status().message()));
  }
  return *type;
}

// Types whose values the backend writes the same way on the wire, so that a column DuckDB
// computed as one of them can be reported as another: all numbers are decimal strings, and
// these textual types are plain strings.
bool SameWireEncoding(const std::string& a, const std::string& b) {
  static const auto* const kGroups = new std::vector<std::set<std::string>>{
      {"INTEGER", "FLOAT", "NUMERIC", "BIGNUMERIC"}, {"STRING", "JSON", "GEOGRAPHY"}};
  if (a == b) {
    return true;
  }
  for (const std::set<std::string>& group : *kGroups) {
    if (group.contains(a) && group.contains(b)) {
      return true;
    }
  }
  return false;
}

// Takes the column names, and the types where it can, from the schema the analyzer resolved,
// which follows BigQuery's typing rules (SUM of INT64 is INT64, an unnamed column is f0_), over
// the one DuckDB reports. The rows are still encoded after DuckDB's types, so a type that
// would change their encoding (DATETIME for a TIMESTAMP, say) is left as DuckDB has it.
std::vector<FieldSchema> ReconcileSchema(std::vector<FieldSchema> duckdb_schema,
                                         const std::vector<FieldSchema>& resolved_schema) {
  if (duckdb_schema.size() != resolved_schema.size()) {
    return duckdb_schema;
  }
  for (size_t i = 0; i < duckdb_schema.size(); ++i) {
    FieldSchema& field = duckdb_schema[i];
    const FieldSchema& resolved = resolved_schema[i];
    field.name = resolved.name;
    if (field.mode != resolved.mode) {
      continue;
    }
    if (field.type == "RECORD" && resolved.type == "RECORD") {
      field.fields = ReconcileSchema(std::move(field.fields), resolved.fields);
    } else if (SameWireEncoding(field.type, resolved.type)) {
      field.type = resolved.type;
    }
  }
  return duckdb_schema;
}

std::string TableName(const TableReference& table) {
  return table.project_id + ":" + table.dataset_id + "." + table.table_id;
}

// Column definitions for a table that holds `schema`, or nullopt when a type has no column
// type of its own in the emulator.
std::optional<std::string> ColumnDefinitions(const std::vector<FieldSchema>& schema) {
  std::string columns;
  for (const FieldSchema& field : schema) {
    if (!columns.empty()) {
      columns += ", ";
    }
    try {
      columns += QuoteIdentifier(field.name) + " " + ToDuckDbType(field.ToJson());
    } catch (const ApiError&) {
      return std::nullopt;
    }
  }
  return columns;
}

}  // namespace

Emulator::Emulator(std::string data_dir) : data_dir_(std::move(data_dir)) {
  if (!data_dir_.empty()) {
    std::filesystem::create_directories(data_dir_);
  }
}

std::string Emulator::ProjectDatabase(const std::string& project_id) const {
  if (data_dir_.empty()) {
    return ":memory:";
  }
  return (std::filesystem::path(data_dir_) / ProjectFileName(project_id)).string();
}

void Emulator::EnsureProject(const std::string& project_id) {
  if (project_id.empty()) {
    throw ApiError::Invalid("Project id is required");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (projects_.contains(project_id)) {
    return;
  }
  const std::string database = ProjectDatabase(project_id);
  try {
    backend_.Execute("ATTACH " + QuoteLiteral(database) + " AS " + QuoteIdentifier(project_id));
  } catch (const BackendError& error) {
    // Most likely another process holds the file's lock.
    throw ApiError::Internal("Failed to open " + database + ": " + error.what());
  }
  projects_.insert(project_id);
}

QueryResult Emulator::Execute(const std::string& sql, const std::vector<std::string>& setup) {
  try {
    return backend_.Execute(sql, setup);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

QueryResult Emulator::Prepare(const std::string& sql, const std::vector<std::string>& setup) {
  try {
    return backend_.Prepare(sql, setup);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

Emulator::Translation Emulator::Translate(const std::string& query,
                                          const QueryParameters& parameters,
                                          const std::string& default_project,
                                          const std::string& default_dataset) {
  AnalyzerSettings settings{.default_project = default_project, .default_dataset = default_dataset};
  googlesql::TypeFactory type_factory;
  for (const FieldSchema& field : parameters.named_types()) {
    settings.named_parameters.emplace_back(field.name, ParameterType(field, type_factory));
  }
  for (const FieldSchema& field : parameters.positional_types()) {
    settings.positional_parameters.push_back(ParameterType(field, type_factory));
  }
  DuckDbTableSource source(backend_);
  BigQueryCatalog catalog(source, &type_factory, settings.default_project,
                          settings.default_dataset);
  const AnalyzerResult analyzed = AnalyzeGoogleSql(query, catalog, type_factory, settings);
  std::string unsupported;
  std::optional<std::string> sql = TranslateToDuckDbSql(
      analyzed.statement(), parameters,
      DefaultDataset{settings.default_project, settings.default_dataset}, &unsupported);
  if (!sql.has_value()) {
    throw ApiError::InvalidQuery("The emulator does not support " + unsupported);
  }
  return {*std::move(sql), analyzed.result_schema()};
}

std::shared_ptr<const Job> Emulator::RunQuery(const QueryRequest& request) {
  EnsureProject(request.project_id);
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->query = request.query;
  job->dry_run = request.dry_run;
  job->destination_table = request.destination_table;
  job->create_disposition = request.create_disposition;
  job->write_disposition = request.write_disposition;
  job->creation_time_ms = NowMillis();

  std::vector<std::string> setup;
  AnalyzerSettings settings{.default_project = request.project_id};
  if (request.default_dataset.has_value()) {
    const std::string dataset_project = request.default_dataset->project_id.empty()
                                            ? request.project_id
                                            : request.default_dataset->project_id;
    EnsureProject(dataset_project);
    setup.push_back("USE " + QualifiedName(DatasetReference{dataset_project,
                                                            request.default_dataset->dataset_id}));
    settings.default_project = dataset_project;
    settings.default_dataset = request.default_dataset->dataset_id;
  } else {
    setup.push_back("USE " + QuoteIdentifier(request.project_id));
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request.job_id.empty()) {
      do {
        job->job_id = "job_" + std::to_string(next_job_number_++);
      } while (jobs_.contains(JobKey(request.project_id, job->job_id)) ||
               running_jobs_.contains(JobKey(request.project_id, job->job_id)));
    } else {
      job->job_id = request.job_id;
    }
    if (!request.dry_run) {
      const std::string key = JobKey(request.project_id, job->job_id);
      if (jobs_.contains(key) || running_jobs_.contains(key)) {
        throw ApiError::Duplicate("Already Exists: Job " + key);
      }
      running_jobs_.insert(key);
    }
  }

  try {
    const Translation translation = Translate(request.query, request.parameters,
                                              settings.default_project, settings.default_dataset);
    if (request.destination_table.has_value() && !translation.schema.has_value()) {
      throw ApiError::Invalid("Cannot set destination table in jobs with DML/DDL statements");
    }
    QueryResult result;
    if (request.dry_run) {
      result = Prepare(translation.sql, setup);
    } else if (request.destination_table.has_value()) {
      result = WriteDestination(request, *request.destination_table, translation.sql,
                                *translation.schema, setup);
    } else {
      result = Execute(translation.sql, setup);
    }
    if (translation.schema.has_value()) {
      result.schema = ReconcileSchema(std::move(result.schema), *translation.schema);
    }
    job->result = std::move(result);
  } catch (const ApiError& error) {
    job->error = error;
  } catch (const std::exception& error) {
    job->error = ApiError::InvalidQuery(error.what());
  }
  job->end_time_ms = NowMillis();

  if (request.dry_run) {
    return job;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  running_jobs_.erase(JobKey(request.project_id, job->job_id));
  jobs_[JobKey(request.project_id, job->job_id)] = job;
  return job;
}

std::shared_ptr<const Job> Emulator::RunLoad(const LoadRequest& request) {
  EnsureProject(request.project_id);
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->job_id = request.job_id;
  job->is_load = true;
  job->load_configuration = request.configuration;
  job->destination_table = request.destination_table;
  job->create_disposition = request.configuration.value("createDisposition", "");
  job->write_disposition = request.configuration.value("writeDisposition", "");
  if (job->write_disposition.empty()) job->write_disposition = "WRITE_APPEND";
  job->creation_time_ms = NowMillis();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (job->job_id.empty()) {
      do {
        job->job_id = "job_" + std::to_string(next_job_number_++);
      } while (jobs_.contains(JobKey(request.project_id, job->job_id)) ||
               running_jobs_.contains(JobKey(request.project_id, job->job_id)));
    }
    const std::string key = JobKey(request.project_id, job->job_id);
    if (jobs_.contains(key) || running_jobs_.contains(key)) {
      throw ApiError::Duplicate("Already Exists: Job " + key);
    }
    running_jobs_.insert(key);
  }

  try {
    const json& config = request.configuration;
    const std::string format = config.value("sourceFormat", "CSV");
    if (format != "CSV" && format != "NEWLINE_DELIMITED_JSON" && format != "PARQUET") {
      throw ApiError::Invalid("Unsupported source format: " + format);
    }
    const json uris = config.value("sourceUris", json::array());
    if (!uris.is_array() || uris.empty()) throw ApiError::Invalid("sourceUris is required");
    DownloadedFiles downloads;
    std::string paths;
    for (const json& item : uris) {
      if (!item.is_string()) throw ApiError::Invalid("Invalid source URI");
      const std::string uri = item.get<std::string>();
      std::string path;
      if (uri.starts_with("gs://")) {
        char pattern[] = "/tmp/bigquery-load-XXXXXX";
        const int fd = mkstemp(pattern);
        if (fd < 0) throw ApiError::Internal("Could not create load temporary file");
        close(fd);
        path = pattern;
        downloads.paths.push_back(path);
        DownloadGcs(uri, path);
      } else if (uri.starts_with("file://")) {
        path = uri.substr(7);
      } else if (uri.find("://") == std::string::npos) {
        path = uri;
      } else {
        throw ApiError::Invalid("Unsupported source URI: " + uri);
      }
      if (path.empty() || !std::filesystem::is_regular_file(path)) {
        throw ApiError::Invalid("Source file does not exist: " + uri);
      }
      paths += (paths.empty() ? "" : ", ") + QuoteLiteral(path);
    }
    const std::string files = "[" + paths + "]";
    std::vector<FieldSchema> requested_schema;
    if (config.contains("schema")) {
      for (const json& field : config.at("schema").at("fields")) {
        requested_schema.push_back(ParseField(field));
      }
    }
    if (requested_schema.empty()) {
      TableReference destination = request.destination_table;
      if (destination.project_id.empty()) destination.project_id = request.project_id;
      try {
        requested_schema = GetTable(destination).schema;
      } catch (const ApiError& error) {
        if (error.http_status() != 404) throw;
      }
    }
    std::string sql;
    if (format == "CSV") {
      sql = "SELECT * FROM read_csv(" + files + ", header=false, skip=" +
            std::to_string(config.value("skipLeadingRows", 0)) + ", delim=" +
            QuoteLiteral(config.value("fieldDelimiter", ","));
      if (!requested_schema.empty()) {
        sql += ", auto_detect=false";
        std::string columns;
        for (const FieldSchema& field : requested_schema) {
          columns += (columns.empty() ? "" : ", ") + QuoteLiteral(field.name) + ": " +
                     QuoteLiteral(ToDuckDbType(field.ToJson()));
        }
        sql += ", columns={" + columns + "}";
      }
      sql += ")";
    } else if (format == "NEWLINE_DELIMITED_JSON") {
      sql = "SELECT * FROM read_json(" + files + ", format='newline_delimited')";
    } else {
      sql = "SELECT * FROM read_parquet(" + files + ")";
    }
    if (!requested_schema.empty() && format != "CSV") {
      std::string columns;
      for (const FieldSchema& field : requested_schema) {
        if (!columns.empty()) columns += ", ";
        columns += "CAST(" + QuoteIdentifier(field.name) + " AS " +
                   ToDuckDbType(field.ToJson()) + ") AS " + QuoteIdentifier(field.name);
      }
      sql = "SELECT " + columns + " FROM (" + sql + ") AS source";
    }
    const QueryResult prepared = Prepare(sql);
    QueryRequest destination_request;
    destination_request.project_id = request.project_id;
    destination_request.create_disposition = job->create_disposition;
    destination_request.write_disposition = job->write_disposition;
    QueryResult result = WriteDestination(destination_request, request.destination_table, sql,
                                          requested_schema.empty() ? prepared.schema : requested_schema,
                                          {}, true);
    job->output_rows = std::stoll(result.rows.at(0).at("f").at(0).at("v").get<std::string>());
    job->result = QueryResult{};
  } catch (const ApiError& error) {
    job->error = error;
  } catch (const std::exception& error) {
    job->error = ApiError::Invalid(error.what());
  }
  job->end_time_ms = NowMillis();
  std::lock_guard<std::mutex> lock(mutex_);
  running_jobs_.erase(JobKey(request.project_id, job->job_id));
  jobs_[JobKey(request.project_id, job->job_id)] = job;
  return job;
}

QueryResult Emulator::WriteDestination(const QueryRequest& request, TableReference destination,
                                       const std::string& sql,
                                       const std::vector<FieldSchema>& schema,
                                       const std::vector<std::string>& setup, bool count_only) {
  const std::string create =
      request.create_disposition.empty() ? "CREATE_IF_NEEDED" : request.create_disposition;
  const std::string write =
      request.write_disposition.empty() ? "WRITE_EMPTY" : request.write_disposition;
  if (create != "CREATE_IF_NEEDED" && create != "CREATE_NEVER") {
    throw ApiError::Invalid("Invalid create disposition: " + create);
  }
  if (write != "WRITE_EMPTY" && write != "WRITE_APPEND" && write != "WRITE_TRUNCATE" &&
      write != "WRITE_TRUNCATE_DATA") {
    throw ApiError::Invalid("Invalid write disposition: " + write);
  }
  std::set<std::string> names;
  std::string duplicates;
  for (const FieldSchema& field : schema) {
    std::string name = field.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (!names.insert(name).second) {
      duplicates += (duplicates.empty() ? "" : ", ") + field.name;
    }
  }
  if (!duplicates.empty()) {
    throw ApiError::InvalidQuery(
        "Duplicate column names in the result are not supported. Found duplicate(s): " +
        duplicates);
  }

  if (destination.project_id.empty()) {
    destination.project_id = request.project_id;
  }
  EnsureProject(destination.project_id);
  GetDataset(DatasetReference{destination.project_id, destination.dataset_id});
  std::optional<TableInfo> existing;
  try {
    existing = GetTable(destination);
  } catch (const ApiError& error) {
    if (error.http_status() != 404) {
      throw;
    }
  }
  if (!existing.has_value() && create == "CREATE_NEVER") {
    throw ApiError::NotFound("Not found: Table " + TableName(destination));
  }
  if (existing.has_value() && write == "WRITE_EMPTY" && existing->num_rows > 0) {
    throw ApiError::Duplicate("Already Exists: Table " + TableName(destination));
  }

  // The result is materialized first, which runs the query once even when it reads the
  // destination itself. DuckDB lets a transaction write to a single database, so the temporary
  // table is filled before the transaction that writes the destination begins.
  const std::string result_table = "temp.main._bigquery_emulator_query_result";
  std::string aliases;
  for (const FieldSchema& field : schema) {
    aliases += (aliases.empty() ? "" : ", ") + QuoteIdentifier(field.name);
  }
  const std::string target = QualifiedName(destination);
  std::vector<std::string> statements = {
      "CREATE TEMP TABLE _bigquery_emulator_query_result AS SELECT * FROM (" + sql +
          ") AS _bigquery_emulator_query_result(" + aliases + ")",
      "BEGIN TRANSACTION"};
  if (!existing.has_value() || write == "WRITE_TRUNCATE") {
    if (existing.has_value()) {
      statements.push_back("DROP TABLE " + target);
    }
    // The columns take BigQuery's types for the result, so the table reads back as the query's
    // schema rather than as whatever DuckDB computed.
    if (const std::optional<std::string> columns = ColumnDefinitions(schema)) {
      statements.push_back("CREATE TABLE " + target + " (" + *columns + ")");
      statements.push_back("INSERT INTO " + target + " SELECT * FROM " + result_table);
    } else {
      statements.push_back("CREATE TABLE " + target + " AS SELECT * FROM " + result_table);
    }
  } else {
    if (write == "WRITE_TRUNCATE_DATA") {
      statements.push_back("DELETE FROM " + target);
    }
    statements.push_back("INSERT INTO " + target + " BY NAME SELECT * FROM " + result_table);
  }
  statements.emplace_back("COMMIT");
  statements.push_back("SELECT " + std::string(count_only ? "count(*)" : "*") + " FROM " +
                       result_table);
  try {
    return backend_.ExecuteAll(statements, setup);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

std::shared_ptr<const Job> Emulator::GetJob(const std::string& project_id,
                                            const std::string& job_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = jobs_.find(JobKey(project_id, job_id));
  if (it == jobs_.end()) {
    throw ApiError::NotFound("Not found: Job " + project_id + ":" + job_id);
  }
  return it->second;
}

std::vector<std::shared_ptr<const Job>> Emulator::ListJobs(const std::string& project_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<const Job>> result;
  for (const auto& entry : jobs_) {
    const auto& job = entry.second;
    if (job->project_id == project_id) {
      result.push_back(job);
    }
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    if (left->creation_time_ms != right->creation_time_ms) {
      return left->creation_time_ms > right->creation_time_ms;
    }
    return left->job_id > right->job_id;
  });
  return result;
}

void Emulator::DeleteJob(const std::string& project_id, const std::string& job_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (jobs_.erase(JobKey(project_id, job_id)) == 0) {
    throw ApiError::NotFound("Not found: Job " + project_id + ":" + job_id);
  }
}

std::vector<std::string> Emulator::ListDatasets(const std::string& project_id) {
  EnsureProject(project_id);
  return FirstColumnStrings(
      Execute("SELECT schema_name FROM information_schema.schemata WHERE catalog_name = " +
              QuoteLiteral(project_id) +
              " AND schema_name NOT IN ('main', 'information_schema', 'pg_catalog')"
              " ORDER BY schema_name"));
}

void Emulator::GetDataset(const DatasetReference& dataset) {
  EnsureProject(dataset.project_id);
  const QueryResult result = Execute(
      "SELECT schema_name FROM information_schema.schemata WHERE catalog_name = " +
      QuoteLiteral(dataset.project_id) + " AND schema_name = " + QuoteLiteral(dataset.dataset_id));
  if (result.rows.empty()) {
    throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" + dataset.dataset_id);
  }
}

void Emulator::CreateDataset(const DatasetReference& dataset) {
  EnsureProject(dataset.project_id);
  try {
    backend_.Execute("CREATE SCHEMA " + QualifiedName(dataset));
  } catch (const BackendError& error) {
    throw ApiError::Duplicate("Already Exists: Dataset " + dataset.project_id + ":" +
                              dataset.dataset_id);
  }
}

void Emulator::DeleteDataset(const DatasetReference& dataset, bool delete_contents) {
  GetDataset(dataset);
  if (!delete_contents && !ListTables(dataset).empty()) {
    throw ApiError::Invalid("Dataset " + dataset.project_id + ":" + dataset.dataset_id +
                            " is still in use");
  }
  Execute("DROP SCHEMA " + QualifiedName(dataset) + (delete_contents ? " CASCADE" : ""));
}

std::vector<std::string> Emulator::ListTables(const DatasetReference& dataset) {
  GetDataset(dataset);
  return FirstColumnStrings(
      Execute("SELECT table_name FROM information_schema.tables WHERE table_catalog = " +
              QuoteLiteral(dataset.project_id) +
              " AND table_schema = " + QuoteLiteral(dataset.dataset_id) + " ORDER BY table_name"));
}

TableInfo Emulator::GetTable(const TableReference& table) {
  const DatasetReference dataset{table.project_id, table.dataset_id};
  GetDataset(dataset);
  TableInfo info;
  info.reference = table;
  try {
    info.schema = backend_.Execute("SELECT * FROM " + QualifiedName(table) + " LIMIT 0").schema;
    const QueryResult count = backend_.Execute("SELECT count(*) FROM " + QualifiedName(table));
    info.num_rows = std::stoll(FirstColumnStrings(count).at(0));
  } catch (const BackendError& error) {
    throw ApiError::NotFound("Not found: Table " + table.project_id + ":" + table.dataset_id + "." +
                             table.table_id);
  }
  return info;
}

void Emulator::CreateTable(const TableReference& table, const json& fields) {
  GetDataset(DatasetReference{table.project_id, table.dataset_id});
  std::string columns;
  for (const json& field : fields) {
    if (!columns.empty()) {
      columns += ", ";
    }
    columns += QuoteIdentifier(field.at("name").get<std::string>()) + " " + ToDuckDbType(field);
    if (field.value("mode", "NULLABLE") == "REQUIRED") {
      columns += " NOT NULL";
    }
  }
  try {
    backend_.Execute("CREATE TABLE " + QualifiedName(table) + " (" + columns + ")");
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Table " + table.project_id + ":" +
                                table.dataset_id + "." + table.table_id);
    }
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::DeleteTable(const TableReference& table) {
  GetTable(table);
  Execute("DROP TABLE " + QualifiedName(table));
}

QueryResult Emulator::ListTableData(const TableReference& table, int64_t start_index,
                                    int64_t max_results) {
  GetTable(table);
  return Execute("SELECT * FROM " + QualifiedName(table) + " LIMIT " + std::to_string(max_results) +
                 " OFFSET " + std::to_string(start_index));
}

std::vector<InsertError> Emulator::InsertTableData(const TableReference& table, const json& rows,
                                                   bool skip_invalid_rows,
                                                   bool ignore_unknown_values) {
  if (!rows.is_array()) {
    throw ApiError::Invalid("rows must be an array");
  }
  const std::vector<FieldSchema> schema = GetTable(table).schema;
  std::vector<InsertError> errors;
  std::vector<std::string> statements;
  std::vector<size_t> indexes;
  for (size_t i = 0; i < rows.size(); ++i) {
    try {
      if (!rows[i].is_object() || !rows[i].contains("json") || !rows[i]["json"].is_object()) {
        throw ApiError::Invalid("Row must contain a json object");
      }
      const json& values = rows[i]["json"];
      for (auto it = values.begin(); it != values.end(); ++it) {
        const bool known = std::any_of(schema.begin(), schema.end(), [&](const FieldSchema& field) {
          return field.name == it.key();
        });
        if (!known && !ignore_unknown_values) {
          throw ApiError::Invalid("Unknown field: " + it.key());
        }
      }
      std::string columns;
      std::string literals;
      for (const FieldSchema& field : schema) {
        if (!columns.empty()) {
          columns += ", ";
          literals += ", ";
        }
        columns += QuoteIdentifier(field.name);
        const auto it = values.find(field.name);
        literals +=
            InsertValue(it == values.end() ? json(nullptr) : *it, field, ignore_unknown_values);
      }
      std::string statement = "INSERT INTO ";
      statement += QualifiedName(table);
      statement += " (";
      statement += columns;
      statement += ") VALUES (";
      statement += literals;
      statement += ")";
      statements.push_back(std::move(statement));
      indexes.push_back(i);
    } catch (const ApiError& error) {
      errors.push_back({i, error.what()});
    }
  }
  if (!skip_invalid_rows && !errors.empty()) {
    return errors;
  }
  try {
    for (const auto& [index, message] : backend_.InsertRows(statements, skip_invalid_rows)) {
      errors.push_back({indexes[index], message});
    }
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
  std::sort(errors.begin(), errors.end(),
            [](const InsertError& a, const InsertError& b) { return a.index < b.index; });
  return errors;
}

}  // namespace bigquery_emulator_duckdb
