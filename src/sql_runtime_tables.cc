#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/strings.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/column_metadata.h"
#include "src/ddl_write.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/function_catalog/catalog.h"
#include "src/location.h"
#include "src/references.h"
#include "src/schema_sql.h"
#include "src/sql_runtime.h"
#include "src/table_comments.h"
#include "src/table_history.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

std::vector<std::string> SqlRuntime::ListTables(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  ExpireTables(backend_, expiration_mutex_, dataset);
  return FirstColumnStrings(Execute(TablesQuery(dataset)));
}

std::vector<TableListEntry> SqlRuntime::ListTableEntries(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  ExpireTables(backend_, expiration_mutex_, dataset);
  const std::string where =
      std::format("WHERE database_name = {} AND schema_name = {}", QuoteLiteral(dataset.project_id),
                  QuoteLiteral(dataset.dataset_id));
  std::vector<TableListEntry> entries;
  const QueryResult result =
      Execute(std::format("SELECT table_name, 'TABLE', comment FROM duckdb_tables() {}"
                          " UNION ALL SELECT view_name, 'VIEW', comment FROM duckdb_views() {}"
                          " ORDER BY 1",
                          where, where));
  for (size_t row = 0; row < result.rows.size(); ++row) {
    TableMetadata metadata = CommentMetadata(result.rows.OptionalString(row, 2));
    const auto view = ParseViewMetadata(result.rows.OptionalString(row, 2));
    entries.push_back({
        .table_id = result.rows.String(row, 0),
        .type = view && view->materialized             ? TableType::kMaterializedView
                : result.rows.String(row, 1) == "VIEW" ? TableType::kView
                : metadata.external_data               ? TableType::kExternal
                : metadata.snapshot.has_value()        ? TableType::kSnapshot
                                                       : TableType::kTable,
        .metadata = std::move(metadata),
    });
  }
  return entries;
}

TableInfo SqlRuntime::GetTable(TableReference table, bool include_row_count, bool expire) {
  table.project_id = ResolveProject(table.project_id);
  const DatasetReference dataset{.project_id = table.project_id, .dataset_id = table.dataset_id};
  const std::string location = GetDataset(dataset).Location();
  if (expire) {
    ExpireTables(backend_, expiration_mutex_, dataset);
  }
  TableInfo info;
  info.reference = table;
  info.location = location;
  if (const auto comment = ViewComment(backend_, table); comment.has_value()) {
    std::optional<ViewMetadata> metadata = ParseViewMetadata(*comment);
    if (!metadata.has_value()) {
      throw ApiError::Invalid("View " + TableName(table) +
                              " was not created by the emulator and has no GoogleSQL definition");
    }
    info.materialized = metadata->materialized;
    info.materialized_source = std::move(metadata->materialized_source);
    info.view_query = std::move(metadata->query);
    info.schema = std::move(metadata->schema);
    info.metadata = std::move(metadata->metadata);
    return info;
  }
  try {
    info.schema = TableSchema(backend_, table);
    info.metadata = CommentMetadata(
        RelationComment(backend_, "duckdb_tables()", "table_name", table).value_or(std::nullopt));
    const QueryResult identity =
        backend_.Execute(std::format("SELECT table_oid FROM duckdb_tables() WHERE database_name = "
                                     "{} AND schema_name = {} AND table_name = {}",
                                     QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id),
                                     QuoteLiteral(table.table_id)));
    if (identity.rows.empty()) {
      throw ApiError::NotFound("Table not found: " + table.table_id);
    }
    info.storage_id = identity.rows.Int64(0, 0);
    if (include_row_count && !info.metadata.external_data) {
      const QueryResult count = backend_.Execute("SELECT count(*) FROM " + QualifiedName(table));
      info.num_rows = count.rows.Int64(0, 0);
    }
  } catch (const BackendError& error) {
    throw ApiError::NotFound("Not found: Table " + table.project_id + ":" + table.dataset_id + "." +
                             table.table_id);
  }
  return info;
}

std::vector<FieldSchema> SqlRuntime::DetectExternalSchema(const GcsExternalData& definition,
                                                          const std::string& table_id) {
  if (!external_tables_) {
    throw ApiError::Unsupported("external tables without an explicit schema");
  }
  std::vector<FieldSchema> schema = external_tables_(backend_)->DetectSchema(definition, table_id);
  SetModesGiven(schema);
  return schema;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a table's columns, then its keys.
std::vector<FieldSchema> SqlRuntime::ExternalSchema(GcsExternalData& definition,
                                                    std::vector<FieldSchema> schema,
                                                    const std::vector<FieldSchema>& declared_keys,
                                                    const std::string& table_id) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  if (schema.empty()) {
    schema = DetectExternalSchema(definition, table_id);
  }
  HivePartitioning* hive = definition.hive_partitioning ? &*definition.hive_partitioning : nullptr;
  if (hive == nullptr) {
    return schema;
  }
  hive->fields.clear();
  if (hive->mode.empty()) {
    return schema;
  }
  if (!external_tables_) {
    throw ApiError::Unsupported("Hive-partitioned external tables without an external provider");
  }
  const std::vector<FieldSchema> keys =
      external_tables_(backend_)->DetectPartitionKeys(definition, declared_keys, table_id);
  // The keys replace fields of the same names.
  std::erase_if(schema, [&keys](const FieldSchema& field) {
    return std::ranges::find(keys, field.name, &FieldSchema::name) != keys.end();
  });
  for (const FieldSchema& key : keys) {
    hive->fields.push_back(key.name);
    schema.push_back(key);
  }
  SetModesGiven(schema);
  return schema;
}

TableInfo SqlRuntime::CreateTable(TableReference table, std::vector<FieldSchema> schema,
                                  const TableMetadata& given) {
  table.project_id = ResolveProject(table.project_id);
  TableMetadata metadata = given;
  // Read the sources before holding the mutex, after the checks that fail without them.
  if (metadata.external_data && (schema.empty() || metadata.external_data->hive_partitioning)) {
    GetDataset(DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
    if (RelationComment(backend_, "duckdb_tables()", "table_name", table) ||
        ViewComment(backend_, table)) {
      throw ApiError::Duplicate("Already Exists: Table " + table.project_id + ":" +
                                table.dataset_id + "." + table.table_id);
    }
    schema = ExternalSchema(*metadata.external_data, std::move(schema), {}, table.table_id);
  }
  std::scoped_lock const lock(expiration_mutex_);
  const DatasetMetadata dataset =
      GetDataset(DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  ExpireTables(backend_, expiration_mutex_,
               DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  std::vector<std::string> columns;
  std::ranges::transform(schema, std::back_inserter(columns), [this](const FieldSchema& field) {
    return ColumnDefinition(field, functions_.timestamp_precision());
  });
  DdlWrite write = CreateTableWrite(
      TableDefinition{.table = table, .schema = schema, .metadata = metadata}, dataset);
  auto defaults = schema;
  ApplyDefaultRoundingMode(defaults, metadata.default_rounding_mode == RoundingMode::kUnspecified
                                         ? dataset.default_rounding_mode
                                         : metadata.default_rounding_mode);
  std::ranges::move(ColumnDefaultStatements(table, defaults),
                    std::back_inserter(write.metadata_statements));
  try {
    backend_.ExecuteDdl("CREATE TABLE " + QualifiedName(table) + " (" + Join(columns, ", ") + ")",
                        write.metadata_statements, write.skip_query);
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Table " + table.project_id + ":" +
                                table.dataset_id + "." + table.table_id);
    }
    throw ApiError::Invalid(error.what());
  }
  return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
}

TableInfo SqlRuntime::CreateView(TableReference table, const json& definition,
                                 const TableMetadata& metadata, bool materialized) {
  std::scoped_lock const lock(expiration_mutex_);
  table.project_id = ResolveProject(table.project_id);
  WriteView(table, definition, metadata, /*replace=*/false, materialized);
  return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
}

TableInfo SqlRuntime::UpdateTable(TableReference table,
                                  const std::optional<std::vector<FieldSchema>>& schema,
                                  const std::optional<json>& view,
                                  const std::optional<TableMetadata>& metadata,
                                  bool rounding_mode_unspecified) {
  std::scoped_lock const lock(expiration_mutex_);
  table.project_id = ResolveProject(table.project_id);
  const TableInfo info = GetTable(table, /*include_row_count=*/false);
  if (info.view_query.has_value()) {
    if (metadata.has_value() && metadata->default_collation.has_value()) {
      throw ApiError::Unsupported("defaultCollation on a view");
    }
    if (schema.has_value()) {
      throw ApiError::Unsupported("changing the schema of a view; change its query instead");
    }
    if (view.has_value() && info.materialized) {
      throw ApiError::Invalid("A materialized view cannot be changed to a logical view");
    }
    if (view.has_value()) {
      // Every view the emulator keeps is GoogleSQL.
      json definition = {{"query", *info.view_query}, {"useLegacySql", false}};
      definition.update(*view);
      TableMetadata updated = metadata.value_or(info.metadata);
      updated.creation_time = info.metadata.creation_time;
      WriteView(table, definition, updated, /*replace=*/true);
    } else if (metadata.has_value()) {
      TableMetadata updated = *metadata;
      updated.creation_time = info.metadata.creation_time;
      Execute(ViewCommentStatement(table, {
                                              .query = *info.view_query,
                                              .schema = info.schema,
                                              .metadata = updated,
                                              .materialized = info.materialized,
                                              .materialized_source = info.materialized_source,
                                          }));
    }
    return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
  }
  if (view.has_value()) {
    throw ApiError::Invalid("Table " + TableName(table) + " is not a view");
  }
  if (info.metadata.external_data && schema.has_value()) {
    throw ApiError::Unsupported("changing the schema of an external table");
  }
  // A snapshot's schema, clustering and default rounding mode cannot change.
  if (info.metadata.snapshot.has_value() &&
      ((schema.has_value() && SchemaToJson(*schema) != SchemaToJson(info.schema)) ||
       (metadata.has_value() &&
        (metadata->clustering != info.metadata.clustering ||
         metadata->default_rounding_mode != info.metadata.default_rounding_mode ||
         metadata->default_collation != info.metadata.default_collation)))) {
    throw ApiError::Invalid(
        "Only table expirationTime, description, friendlyName, labels and tags can be updated for "
        "a snapshot.");
  }
  std::vector<std::string> statements;
  std::optional<std::vector<FieldSchema>> updated_schema = schema;
  if (updated_schema.has_value()) {
    const RoundingMode mode = metadata.value_or(info.metadata).default_rounding_mode;
    ApplyDefaultRoundingMode(info.schema, *updated_schema, mode);
    ApplyDefaultCollation(info.schema, *updated_schema,
                          metadata.value_or(info.metadata).default_collation);
    ValidatePartitioning(metadata.value_or(info.metadata), *updated_schema);
    statements = SchemaUpdateStatements(table, info.schema, *updated_schema);
    // The rows kept from before the table had these columns are not those it has now.
    if (const int64_t creation_time = HistoryCreationTime(table, info.metadata);
        !statements.empty() && creation_time != 0) {
      std::ranges::move(DiscardHistoryStatements(table, creation_time),
                        std::back_inserter(statements));
      statements.push_back(CommitVersionsStatement(table.project_id));
    }
    std::ranges::move(ColumnCommentStatements(table, *updated_schema),
                      std::back_inserter(statements));
  }
  if (metadata.has_value()) {
    TableMetadata updated = *metadata;
    updated.creation_time = info.metadata.creation_time;
    // UNSPECIFIED leaves the stored default intact, but newly added fields above take no mode.
    if (rounding_mode_unspecified) {
      updated.default_rounding_mode = info.metadata.default_rounding_mode;
    }
    statements.push_back(
        TableCommentStatement(table, updated, updated_schema.value_or(info.schema)));
  }
  if (statements.empty()) {
    return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
  }
  try {
    backend_.ExecuteDdl(statements.front(), {statements.begin() + 1, statements.end()}, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
  return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
}

void SqlRuntime::WriteView(TableReference table, const json& definition,
                           const TableMetadata& metadata, bool replace, bool materialized) {
  table.project_id = ResolveProject(table.project_id);
  const DatasetMetadata dataset =
      GetDataset(DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  if (!definition.is_object()) {
    throw ApiError::Invalid("Invalid view definition");
  }
  if (materialized) {
    for (const auto& [name, value] : definition.items()) {
      if (name == "lastRefreshTime" || name == "refreshWatermark") {
        continue;  // Output-only fields.
      }
      if ((name == "enableRefresh" && value == true) ||
          (name == "allowNonIncrementalDefinition" && value == false)) {
        continue;
      }
      if (name != "query") {
        throw ApiError::Unsupported("materializedView field " + name);
      }
    }
  }
  if (!materialized && definition.value("useLegacySql", true)) {
    throw ApiError::Unsupported("legacy SQL views");
  }
  if (definition.contains("userDefinedFunctionResources")) {
    throw ApiError::Unsupported("view userDefinedFunctionResources");
  }
  const std::string query = definition.value("query", "");
  if (query.empty()) {
    throw ApiError::Invalid("View query is required");
  }
  // A view reads only tables in its own dataset's location.
  JobLocation location(dataset.Location());
  try {
    const TranslatedStatement translation =
        Translate(std::string(materialized ? "CREATE MATERIALIZED VIEW "
                              : replace    ? "CREATE OR REPLACE VIEW "
                                           : "CREATE VIEW ") +
                      googlesql::ToIdentifierLiteral(table.project_id + "." + table.dataset_id +
                                                     "." + table.table_id) +
                      " AS " + query,
                  {}, table.project_id, "", &location);
    if (!translation.view.has_value()) {
      throw ApiError::Internal("CREATE VIEW was not translated to a view");
    }
    ViewDefinition view = *translation.view;
    // REST-created views report explicit modes, unlike views created through DDL.
    SetModesGiven(view.schema);
    view.metadata = metadata;
    // A replaced view keeps the expiration `metadata` gives it.
    backend_.ExecuteDdl(
        translation.sql,
        CreateViewWrite(view, replace ? DatasetMetadata{} : dataset).metadata_statements, "");
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Table " + TableName(table));
    }
    throw ApiError::Invalid(error.what());
  } catch (const std::runtime_error& error) {
    throw ApiError::Invalid(error.what());
  }
}

int64_t SqlRuntime::HistoryCreationTime(const TableReference& table,
                                        const TableMetadata& metadata) const {
  return project_ids_.contains(table.project_id) ? metadata.creation_time : 0;
}

std::mutex& SqlRuntime::HistoryMutex(const TableReference& table) {
  if (project_ids_.contains(table.project_id)) {
    SweepHistory(backend_, table.project_id);
  }
  const std::scoped_lock lock(history_mutexes_mutex_);
  std::unique_ptr<std::mutex>& mutex = history_mutexes_[ToLowerAscii(QualifiedName(table))];
  if (!mutex) {
    mutex = std::make_unique<std::mutex>();
  }
  return *mutex;
}

std::unique_lock<std::mutex> SqlRuntime::KeepVersion(Backend& backend,
                                                     const TableReference& table) {
  std::unique_lock<std::mutex> lock;
  if (!backend.InTransaction()) {
    lock = std::unique_lock(HistoryMutex(table));
  }
  const auto comment = RelationComment(backend, "duckdb_tables()", "table_name", table);
  const int64_t creation_time =
      comment.has_value() ? HistoryCreationTime(table, CommentMetadata(*comment)) : 0;
  if (creation_time == 0) {
    return lock;
  }
  const std::vector<std::string> statements = KeepVersionStatements(table, creation_time);
  if (backend.InTransaction()) {
    backend.ExecuteAll(statements);
    backend.BeforeCommit(CommitVersionsStatement(table.project_id));
    return lock;
  }
  const auto session = backend_.NewSession();
  session->Transaction("BEGIN TRANSACTION");
  session->ExecuteAll(statements);
  session->BeforeCommit(CommitVersionsStatement(table.project_id));
  session->Transaction("COMMIT");
  return lock;
}

void RejectReadOnlyWrite(const TableInfo& table) {
  if (table.metadata.external_data) {
    throw ApiError::Invalid("Cannot write to external table " + TableName(table.reference));
  }
  if (table.metadata.snapshot.has_value()) {
    throw ApiError::Invalid("Table " + TableName(table.reference) +
                            " is a snapshot, and snapshots are immutable.");
  }
}

void SqlRuntime::DeleteTable(TableReference table) {
  table.project_id = ResolveProject(table.project_id);
  ExpireTables(backend_, expiration_mutex_,
               DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  // Views the emulator did not create have no metadata for GetTable, but can still be dropped.
  if (ViewComment(backend_, table).has_value()) {
    Execute("DROP VIEW " + QualifiedName(table));
    return;
  }
  GetTable(table, false);
  Execute("DROP TABLE " + QualifiedName(table));
}

}  // namespace bigquery_emulator_duckdb
