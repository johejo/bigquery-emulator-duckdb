#include "src/duckdb_catalog.h"

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
         " WHERE lower(dataset_id) = lower(" + QuoteLiteral(graph.dataset_id) +
         ") AND lower(graph_id) = lower(" + QuoteLiteral(graph.table_id) + ")";
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
      "SELECT schema_name FROM information_schema.schemata WHERE catalog_name = {}"
      " AND schema_name NOT IN {} ORDER BY schema_name",
      QuoteLiteral(project), kDuckDbSchemas);
}

// The datasets of `project` by name, each with the metadata recorded for it, or only `dataset`.
std::string DatasetEntriesQuery(const std::string& project,
                                const std::optional<std::string>& dataset) {
  return std::format(
      "SELECT schema_name, (SELECT metadata FROM {} WHERE dataset_id = schema_name)"
      " FROM information_schema.schemata WHERE catalog_name = {} AND schema_name NOT IN {}{}"
      " ORDER BY schema_name",
      DatasetMetadataTable(project), QuoteLiteral(project), kDuckDbSchemas,
      dataset.has_value() ? " AND schema_name = " + QuoteLiteral(*dataset) : "");
}

DatasetMetadata ParseDatasetMetadata(const std::optional<std::string>& recorded) {
  if (!recorded.has_value()) {
    return {};
  }
  json object = json::parse(*recorded);
  std::vector<std::string> label_order = object.value("labelOrder", std::vector<std::string>{});
  object.erase("labelOrder");
  DatasetMetadata metadata = DatasetMetadataFromJson(object);
  metadata.label_order = std::move(label_order);
  if (object.contains("creationTime") && object["creationTime"].is_string()) {
    metadata.creation_time = std::stoll(object["creationTime"].get<std::string>());
  }
  return metadata;
}

// The tables and views of `dataset` by name.
std::string TablesQuery(const DatasetReference& dataset) {
  return std::format(
      "SELECT table_name FROM information_schema.tables WHERE table_catalog = {}"
      " AND table_schema = {} ORDER BY table_name",
      QuoteLiteral(dataset.project_id), QuoteLiteral(dataset.dataset_id));
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
      "SELECT database_name, schema_name, table_name, comment, 'TABLE' FROM duckdb_tables()"
      " WHERE comment IS NOT NULL{}"
      " UNION ALL SELECT database_name, schema_name, view_name, comment, 'VIEW' FROM duckdb_views()"
      " WHERE comment IS NOT NULL{}",
      where, where));
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
        view ? "duckdb_views()" : "duckdb_tables()", QuoteLiteral(table.project_id),
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
      "SELECT comment FROM {} WHERE database_name = {} AND schema_name = {} AND {} = {}", relations,
      QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id), name,
      QuoteLiteral(table.table_id)));
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
std::vector<FieldSchema> TableSchema(Backend& backend, const TableReference& table) {
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
    throw ApiError::Invalid(*error + "; change the column's type to read the table");
  }
  return schema;
}

std::string ReadTableRelation(Backend& backend, const TableReference& table) {
  const std::string relation = QualifiedName(table);
  const auto converted = backend.NumericReadQuery(relation);
  return converted ? "(" + *converted + ") AS _bq_storage" : relation;
}

void CheckWritableTable(Backend& backend, const TableReference& table) {
  if (backend.NumericReadQuery(QualifiedName(table)).has_value()) {
    throw ApiError::Unsupported("writing to external numeric storage in " + TableName(table) +
                                "; copy the table first");
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
