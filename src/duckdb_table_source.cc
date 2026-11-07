#include "src/duckdb_table_source.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/external_table_provider.h"
#include "src/field_schema.h"
#include "src/location.h"
#include "src/partition_policy_read.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/statement_resources.h"
#include "src/table_comments.h"
#include "src/table_history.h"
#include "src/table_metadata.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {

namespace {

// Serves the analyzer the tables the emulator keeps in DuckDB. Each table, dataset, routine and
// connection it finds counts as one the job uses, in `location` when it is set.
class DuckDbTableSource : public TableSource {
 public:
  // `project_ids` maps each project's ID and number to its ID, as Emulator::ResolveProject does.
  DuckDbTableSource(Backend& backend,
                    const std::unordered_map<std::string, std::string>& project_ids,
                    JobLocation* location, std::unique_ptr<ExternalTableProvider> external,
                    std::optional<int64_t> partition_cutoff_micros)
      : backend_(backend),
        external_(std::move(external)),
        project_ids_(project_ids),
        location_(location),
        partition_cutoff_micros_(partition_cutoff_micros) {}

  DatasetReference ResolveDatasetReference(const DatasetReference& dataset) override {
    const std::vector<std::string> key{dataset.project_id, dataset.dataset_id};
    if (const auto it = datasets_.find(key); it != datasets_.end()) {
      return it->second;
    }
    if (const auto found = FindDatasetReference(backend_, dataset)) {
      datasets_.emplace(key, *found);
      datasets_.emplace(std::vector<std::string>{found->project_id, found->dataset_id}, *found);
      return *found;
    }
    return dataset;
  }

  TableReference ResolveTableReference(const TableReference& table) override {
    const std::vector<std::string> key{table.project_id, table.dataset_id, table.table_id};
    if (const auto it = tables_.find(key); it != tables_.end()) {
      return it->second;
    }
    if (const auto found = FindTableReference(backend_, table)) {
      tables_.emplace(key, *found);
      tables_.emplace(
          std::vector<std::string>{found->project_id, found->dataset_id, found->table_id}, *found);
      return *found;
    }
    const auto dataset =
        ResolveDatasetReference({.project_id = table.project_id, .dataset_id = table.dataset_id});
    TableReference resolved = table;
    resolved.dataset_id = dataset.dataset_id;
    CheckCaseInsensitiveTableName(backend_, resolved);
    return resolved;
  }

  std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                    const std::string& dataset,
                                                    const std::string& table) override {
    if (IsDuckDbSchema(dataset)) {
      return std::nullopt;
    }
    try {
      std::vector<FieldSchema> schema = ResolvedTableSchema(
          backend_,
          ResolveTableReference({.project_id = project, .dataset_id = dataset, .table_id = table}));
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

  std::optional<TableRead> ReadQuery(const TableReference& table) override {
    const auto comment = RelationComment(backend_, "duckdb_tables()", "table_name", table);
    const auto metadata = comment ? CommentMetadata(*comment) : TableMetadata{};
    RequirePolicySnapshot(table, metadata);
    if (metadata.external_data) {
      if (!external_) {
        throw ApiError::UnsupportedQuery("external table reads without an external provider");
      }
      return external_->Read(*metadata.external_data, TableSchema(backend_, table),
                             table.project_id + "." + table.dataset_id + "." + table.table_id);
    }
    if (partition_cutoff_micros_.has_value() && UsesPartitionPolicy(table, metadata)) {
      auto query = PartitionPolicyTableReadQuery(backend_, table, *partition_cutoff_micros_,
                                                 PartitionRowVersion::kCurrent, true);
      if (!query.ok()) {
        throw ApiError::QueryFailure(std::string(query.status().message()));
      }
      return TableRead(*std::move(query));
    }
    const auto query = TableReadQuery(backend_, table);
    return query ? std::optional<TableRead>(*query) : std::nullopt;
  }

  std::optional<std::string> DmlTargetFilter(const TableReference& table,
                                             const std::string& alias) override {
    const auto comment = RelationComment(backend_, "duckdb_tables()", "table_name", table);
    const auto metadata = comment ? CommentMetadata(*comment) : TableMetadata{};
    RequirePolicySnapshot(table, metadata);
    if (!partition_cutoff_micros_ || !metadata.time_partitioning ||
        !UsesPartitionPolicy(table, metadata)) {
      return std::nullopt;
    }
    auto rows = PartitionPolicyTableReadQuery(backend_, table, *partition_cutoff_micros_,
                                              PartitionRowVersion::kCurrent, true);
    if (!rows.ok()) {
      throw ApiError::QueryFailure(std::string(rows.status().message()));
    }
    // Visibility is constant for a partition key, including NULL and special partitions.
    // A semijoin on that key preserves duplicate physical targets without relying on rowid,
    // which a user column may shadow. The read also revalidates the storage identity.
    const std::string key = QuoteIdentifier(metadata.time_partitioning->field);
    return "EXISTS (SELECT 1 FROM (" + *rows + ") AS _bq_visible_target WHERE " +
           "_bq_visible_target." + key + " IS NOT DISTINCT FROM " + QuoteIdentifier(alias) + "." +
           key + ")";
  }

  TableWriteRestriction WriteRestriction(const TableReference& table) override {
    return TableWriteRestrictionOf(backend_, table);
  }

  absl::StatusOr<std::string> TimeTravelQuery(const TableReference& table, int64_t time) override {
    const auto comment = RelationComment(backend_, "duckdb_tables()", "table_name", table);
    if (!comment.has_value() || !project_ids_.contains(table.project_id)) {
      return absl::UnimplementedError("FOR SYSTEM_TIME AS OF a temporary table");
    }
    const auto metadata = CommentMetadata(*comment);
    if (metadata.external_data) {
      return absl::UnimplementedError("FOR SYSTEM_TIME AS OF an external table");
    }
    RequirePolicySnapshot(table, metadata);
    if (partition_cutoff_micros_.has_value() && UsesPartitionPolicy(table, metadata)) {
      return PartitionPolicyTableReadQuery(backend_, table, time, PartitionRowVersion::kHistorical,
                                           true);
    }
    return bigquery_emulator_duckdb::TimeTravelQuery(backend_, table, metadata.creation_time, time);
  }

  std::optional<TableDescription> DescribeTable(const std::string& project,
                                                const std::string& dataset,
                                                const std::string& table) override {
    const TableReference reference =
        ResolveTableReference({.project_id = project, .dataset_id = dataset, .table_id = table});
    if (IsDuckDbSchema(dataset)) {
      return std::nullopt;
    }
    try {
      const auto comment = RelationComment(backend_, "duckdb_tables()", "table_name", reference);
      if (!comment.has_value()) {
        return std::nullopt;
      }
      TableDescription description{
          .schema = ResolvedTableSchema(backend_, reference),
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
    const auto reference = ResolveDatasetReference({.project_id = project, .dataset_id = dataset});
    const QueryResult result = backend_.Execute(DatasetEntriesQuery(project, reference.dataset_id));
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
    std::optional<std::string> location = LocationOf(
        backend_, ResolveDatasetReference({.project_id = project, .dataset_id = dataset}));
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
        TablesQuery(ResolveDatasetReference({.project_id = project, .dataset_id = dataset}))));
  }

  std::optional<std::vector<std::pair<std::optional<std::string>, int64_t>>> CountRows(
      const TableReference& table, const std::string& key) override {
    if (IsDuckDbSchema(table.dataset_id)) {
      return std::nullopt;
    }
    QueryResult result;
    bool policy_read = false;
    try {
      std::string relation = QualifiedName(table);
      const auto comment = RelationComment(backend_, "duckdb_tables()", "table_name", table);
      const auto metadata = comment ? CommentMetadata(*comment) : TableMetadata{};
      RequirePolicySnapshot(table, metadata);
      if (partition_cutoff_micros_.has_value() && UsesPartitionPolicy(table, metadata)) {
        auto rows = PartitionPolicyTableReadQuery(backend_, table, *partition_cutoff_micros_,
                                                  PartitionRowVersion::kCurrent, true);
        if (!rows.ok()) {
          throw ApiError::QueryFailure(std::string(rows.status().message()));
        }
        policy_read = true;
        relation = "(" + *rows + ") AS _bq_partition_counts";
      }
      result = backend_.Execute("SELECT k, COUNT(*) FROM (SELECT " + key + " AS k FROM " +
                                relation + ") GROUP BY k");
    } catch (const BackendError&) {
      // Policy reads have an execution snapshot, so disappearance is not a concurrent drop.
      // In particular, do not turn unsupported staged-policy visibility into empty metadata.
      if (policy_read) {
        throw;
      }
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
        backend_,
        ResolveTableReference({.project_id = project, .dataset_id = dataset, .table_id = table}));
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
    if (!external_) {
      return absl::UnimplementedError("EXTERNAL_QUERY without an external provider");
    }
    return external_->DescribeQuery(key, query, options, *this);
  }

 private:
  void RequirePolicySnapshot(const TableReference& table, const TableMetadata& metadata) const {
    if (!partition_cutoff_micros_ && UsesPartitionPolicy(table, metadata)) {
      throw ApiError::Unsupported(
          "partition expiration without a statement snapshot, including "
          "explicit transactions and query destinations");
    }
  }

  // Whether reads of `table` evaluate partition policies: a native time-partitioned table whose
  // storage has ever had one. History is per storage, so a table without it never expired rows,
  // and an unfiltered read does not depend on the table's identity during its statement.
  [[nodiscard]] bool UsesPartitionPolicy(const TableReference& table,
                                         const TableMetadata& metadata) const {
    if (!project_ids_.contains(table.project_id) || !metadata.time_partitioning ||
        metadata.external_data || metadata.snapshot || metadata.clone) {
      return false;
    }
    const QueryResult policies = backend_.Execute(
        std::format("SELECT count(*) FROM {}.main.emulator_partition_policy_history p "
                    "JOIN {} t ON p.storage_id = t.table_oid WHERE t.database_name = {} "
                    "AND t.schema_name = {} AND t.table_name = {}",
                    QuoteIdentifier(table.project_id), LogicalCatalog("duckdb_tables()"),
                    QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id),
                    QuoteLiteral(table.table_id)));
    return policies.rows.Int64(0, 0) != 0;
  }

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
  std::unique_ptr<ExternalTableProvider> external_;
  const std::unordered_map<std::string, std::string>& project_ids_;
  JobLocation* location_;
  std::optional<int64_t> partition_cutoff_micros_;
  // Successful name resolutions, shared by analysis and translation of this statement.
  // A new source for each statement keeps DDL and dataset option changes visible; do not
  // cache misses, since temporary relations can be introduced during translation.
  std::map<std::vector<std::string>, DatasetReference> datasets_;
  std::map<std::vector<std::string>, TableReference> tables_;
  // DatasetLocation's answers, by project:dataset.
  std::map<std::string, std::string> locations_;
};

}  // namespace

std::unique_ptr<TableSource> NewDuckDbTableSource(
    Backend& backend, const std::unordered_map<std::string, std::string>& project_ids,
    JobLocation* location, std::unique_ptr<ExternalTableProvider> external,
    std::optional<int64_t> partition_cutoff_micros) {
  return std::make_unique<DuckDbTableSource>(backend, project_ids, location, std::move(external),
                                             partition_cutoff_micros);
}

}  // namespace bigquery_emulator_duckdb
