#include "src/duckdb_table_source.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/external_query.h"
#include "src/field_schema.h"
#include "src/location.h"
#include "src/mysql_query.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/spanner_query.h"
#include "src/table_comments.h"
#include "src/table_history.h"
#include "src/table_metadata.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {

namespace {

// The location of the connection `key`, as ConnectionKey gives it: PROJECT.LOCATION.CONNECTION,
// where only the project may have dots.
std::optional<std::string> ConnectionLocation(const std::string& key) {
  const size_t connection = key.rfind('.');
  const size_t location = connection == std::string::npos || connection == 0
                              ? std::string::npos
                              : key.rfind('.', connection - 1);
  if (location == std::string::npos) {
    return std::nullopt;
  }
  return CanonicalLocation(key.substr(location + 1, connection - location - 1));
}

// Serves the analyzer the tables the emulator keeps in DuckDB. Each table, dataset, routine and
// connection it finds counts as one the job uses, in `location` when it is set.
class DuckDbTableSource : public TableSource {
 public:
  // `project_ids` maps each project's ID and number to its ID, as Emulator::ResolveProject does.
  DuckDbTableSource(Backend& backend, ExternalConnections& connections,
                    const std::unordered_map<std::string, std::string>& project_ids,
                    JobLocation* location)
      : backend_(backend),
        connections_(connections),
        project_ids_(project_ids),
        location_(location) {}

  std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                    const std::string& dataset,
                                                    const std::string& table) override {
    if (IsDuckDbSchema(dataset)) {
      return std::nullopt;
    }
    try {
      std::vector<FieldSchema> schema = TableSchema(
          backend_,
          TableReference{.project_id = project, .dataset_id = dataset, .table_id = table});
      Use(project, dataset);
      return schema;
    } catch (const BackendError&) {
      return std::nullopt;
    }
  }

  std::optional<std::string> FindPropertyGraph(const TableReference& graph) override {
    if (IsDuckDbSchema(graph.dataset_id) || !HasProject(graph.project_id)) {
      return std::nullopt;
    }
    const auto result = backend_.Execute(PropertyGraphQuery(graph));
    if (result.rows.empty()) {
      return std::nullopt;
    }
    Use(graph.project_id, graph.dataset_id);
    return result.rows.String(0, 0);
  }

  std::optional<std::string> ReadQuery(const TableReference& table) override {
    return TableReadQuery(backend_, table);
  }

  TableWriteRestriction WriteRestriction(const TableReference& table) override {
    return TableWriteRestrictionOf(backend_, table);
  }

  absl::StatusOr<std::string> TimeTravelQuery(const TableReference& table, int64_t time) override {
    const auto comment = RelationComment(backend_, "duckdb_tables()", "table_name", table);
    if (!comment.has_value() || !project_ids_.contains(table.project_id)) {
      return absl::UnimplementedError("FOR SYSTEM_TIME AS OF a temporary table");
    }
    return bigquery_emulator_duckdb::TimeTravelQuery(backend_, table,
                                                     CommentMetadata(*comment).creation_time, time);
  }

  std::optional<TableDescription> DescribeTable(const std::string& project,
                                                const std::string& dataset,
                                                const std::string& table) override {
    const TableReference reference{.project_id = project, .dataset_id = dataset, .table_id = table};
    if (IsDuckDbSchema(dataset)) {
      return std::nullopt;
    }
    try {
      const auto comment = RelationComment(backend_, "duckdb_tables()", "table_name", reference);
      if (!comment.has_value()) {
        return std::nullopt;
      }
      TableDescription description{
          .schema = TableSchema(backend_, reference),
          .metadata = CommentMetadata(*comment),
      };
      Use(project, dataset);
      return description;
    } catch (const BackendError&) {
      return std::nullopt;
    }
  }

  std::optional<DatasetMetadata> DescribeDataset(const std::string& project,
                                                 const std::string& dataset) override {
    if (IsDuckDbSchema(dataset)) {
      return std::nullopt;
    }
    const QueryResult result = backend_.Execute(DatasetEntriesQuery(project, dataset));
    if (result.rows.empty()) {
      return std::nullopt;
    }
    DatasetMetadata metadata = ParseDatasetMetadata(result.rows.OptionalString(0, 1));
    Use(project, dataset);
    return metadata;
  }

  std::vector<std::string> ListDatasets(const std::string& project) override {
    return FirstColumnStrings(backend_.Execute(DatasetsQuery(project)));
  }

  bool HasProject(const std::string& project) override {
    return !backend_
                .Execute("SELECT 1 FROM duckdb_databases() WHERE database_name = " +
                         QuoteLiteral(project))
                .rows.empty();
  }

  [[nodiscard]] std::string ProjectId(const std::string& project) const override {
    const auto found = project_ids_.find(project);
    return found == project_ids_.end() ? project : found->second;
  }

  std::optional<std::string> DatasetLocation(const std::string& project,
                                             const std::string& dataset) override {
    if (IsDuckDbSchema(dataset)) {
      return std::nullopt;
    }
    const std::string key = project + ":" + dataset;
    if (const auto it = locations_.find(key); it != locations_.end()) {
      return it->second;
    }
    std::optional<std::string> location =
        LocationOf(backend_, {.project_id = project, .dataset_id = dataset});
    if (location.has_value()) {
      locations_[key] = *location;
    }
    return location;
  }

  void ReferenceLocation(const std::string& name, const std::string& location) override {
    if (location_ != nullptr) {
      location_->Reference(name, location);
    }
  }

  std::vector<std::string> ListTables(const std::string& project,
                                      const std::string& dataset) override {
    return FirstColumnStrings(backend_.Execute(
        TablesQuery(DatasetReference{.project_id = project, .dataset_id = dataset})));
  }

  std::optional<std::vector<std::pair<std::optional<std::string>, int64_t>>> CountRows(
      const TableReference& table, const std::string& key) override {
    if (IsDuckDbSchema(table.dataset_id)) {
      return std::nullopt;
    }
    QueryResult result;
    try {
      result = backend_.Execute("SELECT k, COUNT(*) FROM (SELECT " + key + " AS k FROM " +
                                QualifiedName(table) + ") GROUP BY k");
    } catch (const BackendError&) {
      // Dropped since it was listed.
      return std::nullopt;
    }
    std::vector<std::pair<std::optional<std::string>, int64_t>> counts;
    counts.reserve(result.rows.size());
    for (size_t row = 0; row < result.rows.size(); ++row) {
      counts.emplace_back(result.rows.OptionalString(row, 0), result.rows.Int64(row, 1));
    }
    return counts;
  }

  std::optional<ViewDescription> FindView(const std::string& project, const std::string& dataset,
                                          const std::string& table) override {
    const auto comment = ViewComment(
        backend_, TableReference{.project_id = project, .dataset_id = dataset, .table_id = table});
    if (!comment.has_value()) {
      return std::nullopt;
    }
    std::optional<ViewMetadata> metadata = ParseViewMetadata(*comment);
    if (!metadata.has_value()) {
      return ViewDescription{};
    }
    return ViewDescription{
        .query = std::move(metadata->query),
        .metadata = std::move(metadata->metadata),
        .materialized = metadata->materialized,
        .materialized_source = std::move(metadata->materialized_source),
    };
  }

  std::vector<Routine> ListRoutines(const DatasetReference& dataset) override {
    std::vector<Routine> routines;
    if (IsDuckDbSchema(dataset.dataset_id)) {
      return routines;
    }
    const QueryResult result = backend_.Execute(RoutinesQuery(dataset));
    for (size_t row = 0; row < result.rows.size(); ++row) {
      if (auto routine = ParseRoutineComment(
              {
                  .project_id = dataset.project_id,
                  .dataset_id = dataset.dataset_id,
                  .routine_id = result.rows.String(row, 0),
              },
              result.rows.OptionalString(row, 1))) {
        routines.push_back(*std::move(routine));
      }
    }
    return routines;
  }

  std::optional<Routine> FindRoutine(const RoutineReference& routine) override {
    if (IsDuckDbSchema(routine.dataset_id)) {
      return std::nullopt;
    }
    const QueryResult result = backend_.Execute(RoutineQuery(routine));
    if (result.rows.empty()) {
      return std::nullopt;
    }
    std::optional<Routine> found = ParseRoutineComment(routine, result.rows.OptionalString(0, 0));
    Use(routine.project_id, routine.dataset_id);
    return found;
  }

  // NOLINTBEGIN(bugprone-easily-swappable-parameters): a connection and its query options.
  absl::StatusOr<std::vector<FieldSchema>> DescribeExternalQuery(
      const std::string& key, const std::string& query, const std::string& options) override {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto connection = connections_.conninfo.find(key);
    if (connection == connections_.conninfo.end()) {
      return absl::NotFoundError("Not found: Connection " + key);
    }
    if (connection->second == kCloudResourceConnection) {
      return absl::InvalidArgumentError("Connection " + key +
                                        " is not a Cloud SQL or Spanner connection");
    }
    if (const std::optional<std::string> location = ConnectionLocation(key); location.has_value()) {
      ReferenceLocation("Connection " + key, *location);
    }
    const bool spanner = connection->second.starts_with("spanner://");
    const bool mysql = connection->second.starts_with("mysql:");
    absl::StatusOr<std::vector<FieldSchema>> columns =
        [&]() -> absl::StatusOr<std::vector<FieldSchema>> {
      if (spanner) {
        return DescribeSpannerQuery(connection->second.substr(10), query, options);
      }
      const auto decimal_type = DecimalColumnType(options);
      if (!decimal_type.ok()) {
        return decimal_type.status();
      }
      if (mysql) {
        return DescribeMySqlQuery(connection->second.substr(6), query, *decimal_type);
      }
      return DescribePostgresQuery(connection->second, query, *decimal_type);
    }();
    if (!columns.ok()) {
      return columns;
    }
    // Registered once, when first queried rather than at startup, when the database may not
    // be up yet. The shared backend registers it outside any session's transaction.
    const std::scoped_lock lock(connections_.mutex);
    if (!connections_.attached.contains(key)) {
      try {
        const std::string name = QuoteIdentifier(ExternalDatabaseName(key));
        std::string read;
        if (spanner) {
          read = "bq_spanner_query(" + QuoteLiteral(connection->second.substr(10)) + ", sql)";
        } else if (mysql) {
          connections_.backend.Execute("ATTACH " + QuoteLiteral(connection->second.substr(6)) +
                                       " AS " + name + " (TYPE mysql, READ_ONLY)");
          read = "mysql_query(" + QuoteLiteral(ExternalDatabaseName(key)) + ", sql)";
        } else {
          connections_.backend.Execute("ATTACH " + QuoteLiteral(connection->second) + " AS " +
                                       name + " (TYPE postgres, READ_ONLY)");
          read = "postgres_query(" + QuoteLiteral(ExternalDatabaseName(key)) + ", sql)";
        }
        connections_.backend.Execute("CREATE MACRO memory.main." + name +
                                     "(sql) AS TABLE SELECT * FROM " + read);
      } catch (const BackendError& error) {
        return absl::InvalidArgumentError(error.what());
      }
      connections_.attached.insert(key);
    }
    return columns;
  }

 private:
  // Records that the job uses `project`.`dataset`, which exists.
  void Use(const std::string& project, const std::string& dataset) {
    if (location_ == nullptr) {
      return;
    }
    if (const std::optional<std::string> location = DatasetLocation(project, dataset)) {
      location_->Reference("Dataset " + project + ":" + dataset, *location);
    }
  }

  Backend& backend_;
  ExternalConnections& connections_;
  const std::unordered_map<std::string, std::string>& project_ids_;
  JobLocation* location_;
  // DatasetLocation's answers, by project:dataset.
  std::map<std::string, std::string> locations_;
};

}  // namespace

std::unique_ptr<TableSource> NewDuckDbTableSource(
    Backend& backend, ExternalConnections& connections,
    const std::unordered_map<std::string, std::string>& project_ids, JobLocation* location) {
  return std::make_unique<DuckDbTableSource>(backend, connections, project_ids, location);
}

}  // namespace bigquery_emulator_duckdb
