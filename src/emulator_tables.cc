#include <algorithm>
#include <format>
#include <iterator>
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
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/location.h"
#include "src/references.h"
#include "src/schema_sql.h"
#include "src/table_comments.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

std::vector<std::string> Emulator::ListTables(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  ExpireTables(backend_, expiration_mutex_, dataset);
  return FirstColumnStrings(Execute(TablesQuery(dataset)));
}

std::vector<TableListEntry> Emulator::ListTableEntries(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  ExpireTables(backend_, expiration_mutex_, dataset);
  const std::string where =
      std::format("WHERE database_name = {} AND schema_name = {}", QuoteLiteral(dataset.project_id),
                  QuoteLiteral(dataset.dataset_id));
  std::vector<TableListEntry> entries;
  for (const json& row :
       Execute(std::format("SELECT table_name, 'TABLE', comment FROM duckdb_tables() {}"
                           " UNION ALL SELECT view_name, 'VIEW', comment FROM duckdb_views() {}"
                           " ORDER BY 1",
                           where, where))
           .rows) {
    // Keep the wire-field decoding beside the named result fields.
    // cppcheck-suppress useStlAlgorithm
    entries.push_back({
        .table_id = row["f"][0]["v"].get<std::string>(),
        .type = row["f"][1]["v"] == "VIEW" ? TableType::kView : TableType::kTable,
        .metadata = CommentMetadata(row["f"][2]["v"]),
    });
  }
  return entries;
}

TableInfo Emulator::GetTable(TableReference table, bool include_row_count, bool expire) {
  table.project_id = ResolveProject(table.project_id);
  const DatasetReference dataset{.project_id = table.project_id, .dataset_id = table.dataset_id};
  const std::string location = GetDataset(dataset).Location();
  if (expire) {
    ExpireTables(backend_, expiration_mutex_, dataset);
  }
  TableInfo info;
  info.reference = table;
  info.location = location;
  if (const std::optional<json> comment = ViewComment(backend_, table); comment.has_value()) {
    std::optional<ViewMetadata> metadata = ParseViewMetadata(*comment);
    if (!metadata.has_value()) {
      throw ApiError::Invalid("View " + TableName(table) +
                              " was not created by the emulator and has no GoogleSQL definition");
    }
    info.view_query = std::move(metadata->query);
    info.schema = std::move(metadata->schema);
    info.metadata = std::move(metadata->metadata);
    return info;
  }
  try {
    info.schema = TableSchema(backend_, table);
    info.metadata = CommentMetadata(
        RelationComment(backend_, "duckdb_tables()", "table_name", table).value_or(nullptr));
    const QueryResult identity =
        backend_.Execute(std::format("SELECT table_oid FROM duckdb_tables() WHERE database_name = "
                                     "{} AND schema_name = {} AND table_name = {}",
                                     QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id),
                                     QuoteLiteral(table.table_id)));
    if (identity.rows.empty()) {
      throw ApiError::NotFound("Table not found: " + table.table_id);
    }
    info.storage_id = std::stoll(FirstColumnStrings(identity).at(0));
    if (include_row_count) {
      const QueryResult count = backend_.Execute("SELECT count(*) FROM " + QualifiedName(table));
      info.num_rows = std::stoll(FirstColumnStrings(count).at(0));
    }
  } catch (const BackendError& error) {
    throw ApiError::NotFound("Not found: Table " + table.project_id + ":" + table.dataset_id + "." +
                             table.table_id);
  }
  return info;
}

TableInfo Emulator::CreateTable(TableReference table, const std::vector<FieldSchema>& schema,
                                const TableMetadata& metadata) {
  std::scoped_lock const lock(expiration_mutex_);
  table.project_id = ResolveProject(table.project_id);
  const DatasetMetadata dataset =
      GetDataset(DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  ExpireTables(backend_, expiration_mutex_,
               DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  std::vector<std::string> columns;
  std::ranges::transform(schema, std::back_inserter(columns), ColumnDefinition);
  DdlWrite write = CreateTableWrite(
      TableDefinition{.table = table, .schema = schema, .metadata = metadata}, dataset);
  std::ranges::move(ColumnDefaultStatements(table, schema),
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

TableInfo Emulator::CreateView(TableReference table, const json& definition,
                               const TableMetadata& metadata) {
  std::scoped_lock const lock(expiration_mutex_);
  table.project_id = ResolveProject(table.project_id);
  WriteView(table, definition, metadata, /*replace=*/false);
  return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
}

TableInfo Emulator::UpdateTable(TableReference table,
                                const std::optional<std::vector<FieldSchema>>& schema,
                                const std::optional<json>& view,
                                const std::optional<TableMetadata>& metadata) {
  std::scoped_lock const lock(expiration_mutex_);
  table.project_id = ResolveProject(table.project_id);
  const TableInfo info = GetTable(table, /*include_row_count=*/false);
  if (info.view_query.has_value()) {
    if (schema.has_value()) {
      throw ApiError::Unsupported("changing the schema of a view; change its query instead");
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
      Execute(ViewCommentStatement(
          table, {.query = *info.view_query, .schema = info.schema, .metadata = updated}));
    }
    return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
  }
  if (view.has_value()) {
    throw ApiError::Invalid("Table " + TableName(table) + " is not a view");
  }
  std::vector<std::string> statements;
  std::optional<std::vector<FieldSchema>> updated_schema = schema;
  if (updated_schema.has_value()) {
    // Added NUMERIC and BIGNUMERIC fields take the table's default rounding mode. Whether they
    // take the one the same request sets, or the one it replaces, is not known.
    const std::string& mode = metadata.value_or(info.metadata).default_rounding_mode;
    if (mode != info.metadata.default_rounding_mode) {
      std::vector<FieldSchema> before = *updated_schema;
      std::vector<FieldSchema> after = *updated_schema;
      ApplyDefaultRoundingMode(info.schema, before, info.metadata.default_rounding_mode);
      ApplyDefaultRoundingMode(info.schema, after, mode);
      if (SchemaToJson(before) != SchemaToJson(after)) {
        throw ApiError::Unsupported(
            "changing defaultRoundingMode while adding NUMERIC or BIGNUMERIC fields");
      }
    }
    ApplyDefaultRoundingMode(info.schema, *updated_schema, mode);
    statements = SchemaUpdateStatements(table, info.schema, *updated_schema);
    std::ranges::move(ColumnCommentStatements(table, *updated_schema),
                      std::back_inserter(statements));
  }
  if (metadata.has_value()) {
    TableMetadata updated = *metadata;
    updated.creation_time = info.metadata.creation_time;
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

void Emulator::WriteView(TableReference table, const json& definition,
                         const TableMetadata& metadata, bool replace) {
  table.project_id = ResolveProject(table.project_id);
  const DatasetMetadata dataset =
      GetDataset(DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  if (definition.value("useLegacySql", true)) {
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
        Translate(std::string(replace ? "CREATE OR REPLACE VIEW " : "CREATE VIEW ") +
                      googlesql::ToIdentifierLiteral(table.project_id + "." + table.dataset_id +
                                                     "." + table.table_id) +
                      " AS " + query,
                  {}, table.project_id, "", &location);
    if (!translation.view.has_value()) {
      throw ApiError::Internal("CREATE VIEW was not translated to a view");
    }
    ViewDefinition view = *translation.view;
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

void Emulator::DeleteTable(TableReference table) {
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
