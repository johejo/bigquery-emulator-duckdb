#include "src/duckdb_catalog.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/column_metadata.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/table_comments.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

std::string PropertyGraphsTable(const std::string& project) {
  return QuoteIdentifier(project) + ".main._property_graphs";
}

std::string PropertyGraphQuery(const TableReference& graph) {
  return "SELECT definition, native_sql FROM " + PropertyGraphsTable(graph.project_id) +
         " WHERE dataset_id = " + QuoteLiteral(graph.dataset_id) + " AND lower(graph_id) = lower(" +
         QuoteLiteral(graph.table_id) + ")";
}

std::vector<std::string> FirstColumnStrings(const QueryResult& result) {
  std::vector<std::string> values;
  values.reserve(result.rows.size());
  for (size_t row = 0; row < result.rows.size(); ++row) {
    values.push_back(result.rows.String(row, 0));
  }
  return values;
}

// DuckDB's own schemas, which are not datasets. `main` holds what the emulator records about
// the project's datasets; see DatasetMetadataTable.
constexpr std::string_view kDuckDbSchemas = "('main', 'information_schema', 'pg_catalog')";

bool IsDuckDbSchema(const std::string& name) {
  return name == "main" || name == "information_schema" || name == "pg_catalog";
}

// The datasets of `project`, by name.
std::string DatasetsQuery(const std::string& project) {
  return std::format(
      "SELECT schema_name FROM {} WHERE catalog_name = {}"
      " AND schema_name NOT IN {} ORDER BY schema_name",
      LogicalCatalog("information_schema.schemata"), QuoteLiteral(project), kDuckDbSchemas);
}

// The datasets of `project` by name, each with the metadata recorded for it, or only `dataset`.
std::string DatasetEntriesQuery(const std::string& project,
                                const std::optional<std::string>& dataset) {
  return std::format(
      "SELECT schema_name, (SELECT metadata FROM {} WHERE dataset_id = schema_name)"
      " FROM {} WHERE catalog_name = {} AND schema_name NOT IN {}{}"
      " ORDER BY schema_name",
      DatasetMetadataTable(project), LogicalCatalog("information_schema.schemata"),
      QuoteLiteral(project), kDuckDbSchemas,
      dataset.has_value() ? " AND schema_name = " + QuoteLiteral(*dataset) : "");
}

DatasetMetadata ParseDatasetMetadata(const std::optional<std::string>& recorded) {
  if (!recorded.has_value()) {
    return {};
  }
  json object = json::parse(*recorded);
  std::vector<std::string> label_order = object.value("labelOrder", std::vector<std::string>{});
  object.erase("labelOrder");
  std::vector<std::string> tag_order = object.value("tagOrder", std::vector<std::string>{});
  object.erase("tagOrder");
  DatasetMetadata metadata = DatasetMetadataFromJson(object);
  metadata.label_order = std::move(label_order);
  metadata.tag_order = std::move(tag_order);
  if (object.contains("creationTime") && object["creationTime"].is_string()) {
    metadata.creation_time = std::stoll(object["creationTime"].get<std::string>());
  }
  return metadata;
}

namespace {

bool AsciiName(const std::string& name) {
  return std::ranges::all_of(name, [](unsigned char c) { return c < 128; });
}

bool CaseInsensitiveDataset(Backend& backend, const DatasetReference& dataset) {
  // Native catalogs and script-local databases have no dataset metadata table.
  if (backend
          .Execute(std::format("SELECT 1 FROM information_schema.tables WHERE table_catalog = {}"
                               " AND table_schema = 'main' AND table_name = 'emulator_datasets'",
                               QuoteLiteral(dataset.project_id)))
          .rows.empty()) {
    return false;
  }
  const QueryResult result =
      backend.Execute(DatasetEntriesQuery(dataset.project_id, dataset.dataset_id));
  return !result.rows.empty() &&
         ParseDatasetMetadata(result.rows.OptionalString(0, 1)).is_case_insensitive.value_or(false);
}

}  // namespace

std::optional<DatasetReference> FindDatasetReference(Backend& backend,
                                                     const DatasetReference& dataset) {
  const QueryResult result = backend.Execute(
      std::format("SELECT schema_name FROM {} WHERE catalog_name = {}"
                  " AND lower(schema_name) = lower({}) ORDER BY schema_name = {} DESC",
                  LogicalCatalog("information_schema.schemata"), QuoteLiteral(dataset.project_id),
                  QuoteLiteral(dataset.dataset_id), QuoteLiteral(dataset.dataset_id)));
  for (size_t row = 0; row < result.rows.size(); ++row) {
    DatasetReference candidate{
        .project_id = dataset.project_id,
        .dataset_id = result.rows.String(row, 0),
    };
    if (candidate.dataset_id == dataset.dataset_id || CaseInsensitiveDataset(backend, candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::optional<TableReference> FindTableReference(Backend& backend, const TableReference& table) {
  const auto dataset = FindDatasetReference(
      backend, {.project_id = table.project_id, .dataset_id = table.dataset_id});
  if (!dataset) {
    return std::nullopt;
  }
  const QueryResult result = backend.Execute(
      std::format("SELECT table_name FROM {} WHERE table_catalog = {}"
                  " AND table_schema = {} AND lower(table_name) = lower({})",
                  LogicalCatalog("information_schema.tables"), QuoteLiteral(dataset->project_id),
                  QuoteLiteral(dataset->dataset_id), QuoteLiteral(table.table_id)));
  for (size_t row = 0; row < result.rows.size(); ++row) {
    TableReference candidate{
        .project_id = dataset->project_id,
        .dataset_id = dataset->dataset_id,
        .table_id = result.rows.String(row, 0),
    };
    if (candidate.table_id == table.table_id) {
      return candidate;
    }
    if (CaseInsensitiveDataset(backend, *dataset)) {
      if (!AsciiName(candidate.table_id) || !AsciiName(table.table_id)) {
        throw ApiError::Unsupported("non-ASCII table names in a case-insensitive dataset");
      }
      return candidate;
    }
  }
  return std::nullopt;
}

void CheckCaseInsensitiveTableName(Backend& backend, const TableReference& table) {
  if (!AsciiName(table.table_id) &&
      CaseInsensitiveDataset(backend,
                             {.project_id = table.project_id, .dataset_id = table.dataset_id})) {
    throw ApiError::Unsupported("non-ASCII table names in a case-insensitive dataset");
  }
}

void CheckCaseInsensitiveDataset(Backend& backend, const DatasetReference& dataset,
                                 const DatasetMetadata& metadata) {
  if (!metadata.is_case_insensitive.value_or(false)) {
    return;
  }
  if (!backend
           .Execute(std::format("SELECT 1 FROM {} WHERE catalog_name = {} AND lower(schema_name) = "
                                "lower({}) AND schema_name <> {}",
                                LogicalCatalog("information_schema.schemata"),
                                QuoteLiteral(dataset.project_id), QuoteLiteral(dataset.dataset_id),
                                QuoteLiteral(dataset.dataset_id)))
           .rows.empty()) {
    throw ApiError::Invalid(
        "Cannot enable is_case_insensitive the dataset name is case insensitively duplicate with "
        "other dataset(s)");
  }
  if (!backend
           .Execute(std::format("SELECT 1 FROM {} WHERE table_catalog = {} AND table_schema = {} "
                                "GROUP BY lower(table_name) HAVING count(*) > 1",
                                LogicalCatalog("information_schema.tables"),
                                QuoteLiteral(dataset.project_id), QuoteLiteral(dataset.dataset_id)))
           .rows.empty()) {
    throw ApiError::Invalid(
        "Cannot enable is_case_insensitive because there are case insensitive duplicate table "
        "names in the dataset");
  }
  if (std::ranges::any_of(FirstColumnStrings(backend.Execute(TablesQuery(dataset))),
                          [](const std::string& name) { return !AsciiName(name); })) {
    throw ApiError::Unsupported("non-ASCII table names in a case-insensitive dataset");
  }
}

// The tables and views of `dataset` by name.
std::string TablesQuery(const DatasetReference& dataset) {
  return std::format(
      "SELECT table_name FROM {} WHERE table_catalog = {}"
      " AND table_schema = {} ORDER BY table_name",
      LogicalCatalog("information_schema.tables"), QuoteLiteral(dataset.project_id),
      QuoteLiteral(dataset.dataset_id));
}

// Expiration is lazy: catalog reads remove expired relations before exposing them. Sweep
// before analysis too, so DDL targets, wildcards and INFORMATION_SCHEMA see the same catalog.
void ExpireTables(Backend& backend, std::recursive_mutex& expiration_mutex,
                  const std::optional<DatasetReference>& dataset) {
  // Multiple HTTP requests can discover the same expired relation concurrently. Serialize
  // drops so that their transactions do not conflict with each other.
  const std::string where =
      dataset.has_value()
          ? std::format(" AND database_name = {} AND schema_name = {}",
                        QuoteLiteral(dataset->project_id), QuoteLiteral(dataset->dataset_id))
          : "";
  const QueryResult relations = backend.Execute(std::format(
      "SELECT database_name, schema_name, table_name, comment, 'TABLE' FROM {}"
      " WHERE comment IS NOT NULL{}"
      " UNION ALL SELECT database_name, schema_name, view_name, comment, 'VIEW' FROM {}"
      " WHERE comment IS NOT NULL{}",
      LogicalCatalog("duckdb_tables()"), where, LogicalCatalog("duckdb_views()"), where));
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  for (size_t row = 0; row < relations.rows.size(); ++row) {
    const std::string comment = relations.rows.String(row, 3);
    const auto expiration = CommentMetadata(comment).expiration_time;
    if (!expiration.has_value() || *expiration > now) {
      continue;
    }
    const TableReference table{
        .project_id = relations.rows.String(row, 0),
        .dataset_id = relations.rows.String(row, 1),
        .table_id = relations.rows.String(row, 2),
    };
    const bool view = relations.rows.String(row, 4) == "VIEW";
    // Recheck the comment in the drop's transaction: a concurrent update may have extended
    // the expiration, or another request may have replaced the table since the sweep.
    const std::string skip = std::format(
        "SELECT 1 WHERE NOT EXISTS (SELECT 1 FROM {} WHERE database_name = {}"
        " AND schema_name = {} AND {} = {} AND comment = {})",
        LogicalCatalog(view ? "duckdb_views()" : "duckdb_tables()"), QuoteLiteral(table.project_id),
        QuoteLiteral(table.dataset_id), view ? "view_name" : "table_name",
        QuoteLiteral(table.table_id), QuoteLiteral(comment));
    std::scoped_lock const lock(expiration_mutex);
    backend.ExecuteDdl(
        std::string(view ? "DROP VIEW IF EXISTS " : "DROP TABLE IF EXISTS ") + QualifiedName(table),
        {}, skip);
  }
}

// The comment of `table` in `relations`, duckdb_tables() or duckdb_views(), whose `name` column
// names it. The comment is nullopt when it has none, and nothing is returned when there is no such
// table or view.
std::optional<std::optional<std::string>> RelationComment(Backend& backend,
                                                          std::string_view relations,
                                                          std::string_view name,
                                                          const TableReference& table) {
  const QueryResult result = backend.Execute(std::format(
      "SELECT comment FROM {} WHERE database_name = {} AND schema_name = {} AND {} = {}",
      LogicalCatalog(relations), QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id),
      name, QuoteLiteral(table.table_id)));
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return result.rows.OptionalString(0, 0);
}

std::optional<std::optional<std::string>> ViewComment(Backend& backend,
                                                      const TableReference& table) {
  return RelationComment(backend, "duckdb_views()", "view_name", table);
}

// The BigQuery schema of `table`: the recorded schema for a view, or its columns as DuckDB types
// describe them, each replaced by the field its comment records. Throws BackendError when the
// table does not exist, and ApiError when a column's DuckDB type is one the emulator does not
// support.
std::vector<FieldSchema> TableSchema(Backend& backend, const TableReference& requested) {
  const auto resolved = FindTableReference(backend, requested);
  if (!resolved) {
    throw BackendError("Table not found: " + TableName(requested));
  }
  const TableReference& table = *resolved;
  // Reading a view's schema must not require its execution identity to be configured.
  if (const auto comment = ViewComment(backend, table); comment.has_value()) {
    if (const std::optional<ViewMetadata> metadata = ParseViewMetadata(*comment);
        metadata.has_value()) {
      return metadata->schema;
    }
  }
  std::vector<FieldSchema> schema = backend.Prepare("SELECT * FROM " + QualifiedName(table)).schema;
  const QueryResult columns = backend.Execute(ColumnCommentsQuery(table));
  std::vector<std::optional<std::string>> comments;
  comments.reserve(columns.rows.size());
  for (size_t row = 0; row < columns.rows.size(); ++row) {
    comments.push_back(columns.rows.OptionalString(row, 0));
  }
  schema = ApplyColumnComments(std::move(schema), comments);
  if (const std::optional<std::string> error = backend.UnreadableColumnError(QualifiedName(table));
      error.has_value()) {
    throw ApiError::Invalid(*error);
  }
  return schema;
}

std::optional<std::string> TableReadQuery(Backend& backend, const TableReference& table) {
  return backend.NumericReadQuery(QualifiedName(table));
}

TableWriteRestriction TableWriteRestrictionOf(Backend& backend, const TableReference& table) {
  if (backend.NumericReadQuery(QualifiedName(table)).has_value()) {
    return TableWriteRestriction::kExternalNumericStorage;
  }
  const auto comment = RelationComment(backend, "duckdb_tables()", "table_name", table);
  if (comment && CommentMetadata(*comment).external_data) {
    return TableWriteRestriction::kExternal;
  }
  if (comment && CommentMetadata(*comment).snapshot.has_value()) {
    return TableWriteRestriction::kSnapshot;
  }
  return TableWriteRestriction::kNone;
}

std::string ReadTableRelation(Backend& backend, const TableReference& table) {
  if (TableWriteRestrictionOf(backend, table) == TableWriteRestriction::kExternal) {
    throw ApiError::Unsupported("reading an external table through this API; use a query");
  }
  const auto query = TableReadQuery(backend, table);
  return query ? "(" + *query + ") AS _bq_storage" : QualifiedName(table);
}

void CheckWritableTable(Backend& backend, const TableReference& table) {
  const auto comment = RelationComment(backend, "duckdb_tables()", "table_name", table);
  const auto metadata = comment ? CommentMetadata(*comment) : TableMetadata{};
  if (metadata.time_partitioning && metadata.time_partitioning->expiration_ms) {
    throw ApiError::Unsupported("partition expiration for API writes and query destinations");
  }
  switch (TableWriteRestrictionOf(backend, table)) {
    case TableWriteRestriction::kExternalNumericStorage:
      throw ApiError::Unsupported("writing to external numeric storage in " + TableName(table) +
                                  "; copy the table first");
    case TableWriteRestriction::kExternal:
      throw ApiError::Invalid("Cannot write to external table " + TableName(table));
    case TableWriteRestriction::kSnapshot:
      throw ApiError::Invalid("Table " + TableName(table) +
                              " is a snapshot, and snapshots are immutable.");
    case TableWriteRestriction::kNone:
      return;
  }
}

// The location of `dataset`, as `backend` sees it, or nothing when it does not exist.
std::optional<std::string> LocationOf(Backend& backend, const DatasetReference& dataset) {
  if (IsDuckDbSchema(dataset.dataset_id)) {
    return std::nullopt;
  }
  QueryResult result;
  try {
    result = backend.Execute(DatasetEntriesQuery(dataset.project_id, dataset.dataset_id));
  } catch (const BackendError&) {
    // A database that is no project, such as the one holding a script's temporary tables.
    return std::nullopt;
  }
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return ParseDatasetMetadata(result.rows.OptionalString(0, 1)).Location();
}

}  // namespace bigquery_emulator_duckdb
