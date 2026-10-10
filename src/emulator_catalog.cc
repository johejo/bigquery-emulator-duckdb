#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/catalog.h"
#include "src/column_metadata.h"
#include "src/ddl_write.h"
#include "src/duckdb_sql.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/schema_sql.h"
#include "src/table_comments.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// Routine.definitionBody requires project-qualified references to other routines. Built-in
// namespaces such as NET and SAFE are still allowed.
void CheckApiRoutineBody(const Routine& routine, TableSource& source) {
  std::unique_ptr<googlesql::ParserOutput> parsed;
  const auto status =
      googlesql::ParseExpression(routine.resource.at("definitionBody").get<std::string>(),
                                 googlesql::ParserOptions(GoogleSqlLanguageOptions()), &parsed);
  if (!status.ok()) {
    throw ApiError::Invalid(std::string(status.message()));
  }
  googlesql::TypeFactory types;
  BigQueryCatalog builtins(source, &types, "", "");
  std::vector<const googlesql::ASTNode*> nodes{parsed->expression()};
  while (!nodes.empty()) {
    const auto* node = nodes.back();
    nodes.pop_back();
    if (const auto* call = node->GetAsOrNull<googlesql::ASTFunctionCall>()) {
      std::vector<std::string> path = SplitTablePath(call->function()->ToIdentifierVector());
      if (!path.empty() && ToLowerAscii(path.front()) == "safe") {
        path.erase(path.begin());
      }
      const googlesql::Function* function = nullptr;
      if (path.size() == 2 && !builtins.FindFunction(path, &function).ok()) {
        throw ApiError::Invalid(
            "The emulator does not support API routine bodies with function references without a "
            "project ID");
      }
    }
    for (int i = 0; i < node->num_children(); ++i) {
      nodes.push_back(node->child(i));
    }
  }
}

std::vector<std::string> FirstColumnStrings(const QueryResult& result) {
  std::vector<std::string> values;
  values.reserve(result.rows.size());
  std::ranges::transform(result.rows, std::back_inserter(values),
                         [](const json& row) { return row["f"][0]["v"].get<std::string>(); });
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
                                const std::optional<std::string>& dataset = std::nullopt) {
  return std::format(
      "SELECT schema_name, (SELECT metadata FROM {} WHERE dataset_id = schema_name)"
      " FROM information_schema.schemata WHERE catalog_name = {} AND schema_name NOT IN {}{}"
      " ORDER BY schema_name",
      DatasetMetadataTable(project), QuoteLiteral(project), kDuckDbSchemas,
      dataset.has_value() ? " AND schema_name = " + QuoteLiteral(*dataset) : "");
}

DatasetMetadata ParseDatasetMetadata(const json& recorded) {
  if (!recorded.is_string()) {
    return {};
  }
  return DatasetMetadataFromJson(json::parse(recorded.get<std::string>()));
}

// The tables and views of `dataset` by name.
std::string TablesQuery(const DatasetReference& dataset) {
  return std::format(
      "SELECT table_name FROM information_schema.tables WHERE table_catalog = {}"
      " AND table_schema = {} ORDER BY table_name",
      QuoteLiteral(dataset.project_id), QuoteLiteral(dataset.dataset_id));
}

// The comment of `table` in `relations`, duckdb_tables() or duckdb_views(), whose `name` column
// names it. The comment is NULL when it has none, and nothing is returned when there is no such
// table or view.
std::optional<json> RelationComment(Backend& backend, std::string_view relations,
                                    std::string_view name, const TableReference& table) {
  const QueryResult result = backend.Execute(std::format(
      "SELECT comment FROM {} WHERE database_name = {} AND schema_name = {} AND {} = {}", relations,
      QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id), name,
      QuoteLiteral(table.table_id)));
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return result.rows.at(0)["f"][0]["v"];
}

std::optional<json> ViewComment(Backend& backend, const TableReference& table) {
  return RelationComment(backend, "duckdb_views()", "view_name", table);
}

// The BigQuery schema of `table`: the recorded schema for a view, or its columns as DuckDB types
// describe them, each replaced by the field its comment records. Throws BackendError when the
// table does not exist.
std::vector<FieldSchema> TableSchema(Backend& backend, const TableReference& table) {
  // Reading a view's schema must not require its execution identity to be configured.
  if (const std::optional<json> comment = ViewComment(backend, table); comment.has_value()) {
    if (const std::optional<ViewMetadata> metadata = ParseViewMetadata(*comment);
        metadata.has_value()) {
      return metadata->schema;
    }
  }
  std::vector<FieldSchema> schema = backend.Prepare("SELECT * FROM " + QualifiedName(table)).schema;
  std::vector<json> comments;
  std::ranges::transform(backend.Execute(ColumnCommentsQuery(table)).rows,
                         std::back_inserter(comments),
                         [](const json& row) { return row["f"][0]["v"]; });
  return ApplyColumnComments(std::move(schema), comments);
}

// Queries see a GEOGRAPHY column as the STRING it is stored as, since the emulator does not
// translate GEOGRAPHY in SQL; tables.get still reports the column as GEOGRAPHY.
void GeographyAsString(std::vector<FieldSchema>& schema) {
  for (FieldSchema& field : schema) {
    if (field.type == FieldType::kGeography) {
      field.type = FieldType::kString;
    }
    GeographyAsString(field.fields);
  }
}

// Serves the analyzer the tables the emulator keeps in DuckDB.
class DuckDbTableSource : public TableSource {
 public:
  explicit DuckDbTableSource(Backend& backend) : backend_(backend) {}

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
      GeographyAsString(schema);
      return schema;
    } catch (const BackendError&) {
      return std::nullopt;
    }
  }

  std::optional<TableDescription> DescribeTable(const std::string& project,
                                                const std::string& dataset,
                                                const std::string& table) override {
    const TableReference reference{.project_id = project, .dataset_id = dataset, .table_id = table};
    if (IsDuckDbSchema(dataset)) {
      return std::nullopt;
    }
    try {
      const std::optional<json> comment =
          RelationComment(backend_, "duckdb_tables()", "table_name", reference);
      if (!comment.has_value()) {
        return std::nullopt;
      }
      return TableDescription{
          .schema = TableSchema(backend_, reference),
          .metadata = CommentMetadata(*comment),
      };
    } catch (const BackendError&) {
      return std::nullopt;
    }
  }

  std::vector<std::string> ListDatasets(const std::string& project) override {
    return FirstColumnStrings(backend_.Execute(DatasetsQuery(project)));
  }

  std::vector<std::string> ListTables(const std::string& project,
                                      const std::string& dataset) override {
    return FirstColumnStrings(backend_.Execute(
        TablesQuery(DatasetReference{.project_id = project, .dataset_id = dataset})));
  }

  std::optional<std::string> FindViewQuery(const std::string& project, const std::string& dataset,
                                           const std::string& table) override {
    const std::optional<json> comment = ViewComment(
        backend_, TableReference{.project_id = project, .dataset_id = dataset, .table_id = table});
    if (!comment.has_value()) {
      return std::nullopt;
    }
    const std::optional<ViewMetadata> metadata = ParseViewMetadata(*comment);
    return metadata.has_value() ? metadata->query : "";
  }

  std::optional<Routine> FindRoutine(const RoutineReference& routine) override {
    if (IsDuckDbSchema(routine.dataset_id)) {
      return std::nullopt;
    }
    const QueryResult result = backend_.Execute(RoutineQuery(routine));
    if (result.rows.empty()) {
      return std::nullopt;
    }
    return ParseRoutineComment(routine, result.rows.at(0)["f"][0]["v"]);
  }

 private:
  Backend& backend_;
};

}  // namespace

void Emulator::CheckDdlTarget(const TranslatedStatement& translation) {
  if (const auto& target = translation.ddl_target_table;
      target.has_value() && IsDuckDbSchema(target->dataset_id)) {
    throw ApiError::NotFound("Not found: Dataset " + target->project_id + ":" + target->dataset_id);
  }
  if (const auto& target = translation.ddl_target_dataset;
      target.has_value() && IsDuckDbSchema(target->dataset_id)) {
    throw ApiError::NotFound("Not found: Dataset " + target->project_id + ":" + target->dataset_id);
  }
  if (const auto& target = translation.ddl_target_routine;
      target.has_value() && IsDuckDbSchema(target->dataset_id)) {
    throw ApiError::NotFound("Not found: Dataset " + target->project_id + ":" + target->dataset_id);
  }
}

// Throws DuckDB's `error` about the macro that records the routine a CREATE or DROP FUNCTION
// names in BigQuery's words, if it says that the routine exists or that it does not.
std::optional<std::vector<std::string>> Emulator::AlterationStatements(
    const TranslatedStatement& translation) {
  if (const auto& alteration = translation.altered_table) {
    const TableReference& table = alteration->table;
    if (ViewComment(backend_, table).has_value()) {
      throw ApiError::Invalid("ALTER TABLE cannot alter view " + TableName(table));
    }
    const std::optional<json> comment =
        RelationComment(backend_, "duckdb_tables()", "table_name", table);
    if (!comment.has_value()) {
      if (alteration->if_exists) {
        return std::vector<std::string>{};
      }
      throw ApiError::NotFound("Not found: Table " + TableName(table));
    }
    return AlterTableStatements(*alteration, TableSchema(backend_, table),
                                CommentMetadata(*comment));
  }
  if (const auto& alteration = translation.altered_dataset) {
    const DatasetReference& dataset = alteration->dataset;
    const QueryResult result = Execute(DatasetEntriesQuery(dataset.project_id, dataset.dataset_id));
    if (result.rows.empty()) {
      if (alteration->if_exists) {
        return std::vector<std::string>{};
      }
      throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" +
                               dataset.dataset_id);
    }
    return AlterDatasetStatements(*alteration,
                                  ParseDatasetMetadata(result.rows.at(0)["f"][1]["v"]));
  }
  return std::nullopt;
}

std::unique_ptr<TableSource> Emulator::NewTableSource(Backend* session) {
  return std::make_unique<DuckDbTableSource>(session != nullptr ? *session : backend_);
}

std::vector<std::string> Emulator::ListDatasets(std::string project_id) {
  project_id = ResolveProject(project_id);
  return FirstColumnStrings(Execute(DatasetsQuery(project_id)));
}

std::vector<DatasetListEntry> Emulator::ListDatasetEntries(std::string project_id) {
  project_id = ResolveProject(project_id);
  std::vector<DatasetListEntry> entries;
  for (const json& row : Execute(DatasetEntriesQuery(project_id)).rows) {
    // Keep the wire-field decoding beside the named result fields.
    // cppcheck-suppress useStlAlgorithm
    entries.push_back({
        .dataset_id = row["f"][0]["v"].get<std::string>(),
        .metadata = ParseDatasetMetadata(row["f"][1]["v"]),
    });
  }
  return entries;
}

DatasetMetadata Emulator::GetDataset(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  const QueryResult result = Execute(DatasetEntriesQuery(dataset.project_id, dataset.dataset_id));
  if (result.rows.empty()) {
    throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" + dataset.dataset_id);
  }
  return ParseDatasetMetadata(result.rows.at(0)["f"][1]["v"]);
}

void Emulator::CreateDataset(DatasetReference dataset, const DatasetMetadata& metadata) {
  dataset.project_id = ResolveProject(dataset.project_id);
  const std::vector<std::string> statements = DatasetMetadataStatements(dataset, metadata);
  try {
    backend_.ExecuteDdl("CREATE SCHEMA " + QualifiedName(dataset), statements, "");
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Dataset " + dataset.project_id + ":" +
                                dataset.dataset_id);
    }
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::UpdateDataset(DatasetReference dataset, const DatasetMetadata& metadata) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  const std::vector<std::string> statements = DatasetMetadataStatements(dataset, metadata);
  try {
    backend_.ExecuteDdl(statements.front(), {statements.begin() + 1, statements.end()}, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::DeleteDataset(DatasetReference dataset, bool delete_contents) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  if (!delete_contents &&
      (!ListTables(dataset).empty() || !Execute(RoutinesQuery(dataset)).rows.empty())) {
    throw ApiError::Invalid("Dataset " + dataset.project_id + ":" + dataset.dataset_id +
                            " is still in use");
  }
  try {
    backend_.ExecuteDdl(
        "DROP SCHEMA " + QualifiedName(dataset) + (delete_contents ? " CASCADE" : ""),
        DropDatasetWrite(dataset).metadata_statements, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

std::vector<std::string> Emulator::ListTables(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  return FirstColumnStrings(Execute(TablesQuery(dataset)));
}

std::vector<TableListEntry> Emulator::ListTableEntries(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
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

TableInfo Emulator::GetTable(TableReference table, bool include_row_count) {
  table.project_id = ResolveProject(table.project_id);
  const DatasetReference dataset{.project_id = table.project_id, .dataset_id = table.dataset_id};
  GetDataset(dataset);
  TableInfo info;
  info.reference = table;
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

void Emulator::CreateTable(TableReference table, const std::vector<FieldSchema>& schema,
                           const TableMetadata& metadata) {
  table.project_id = ResolveProject(table.project_id);
  GetDataset(DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  std::string columns;
  for (const FieldSchema& field : schema) {
    columns += (columns.empty() ? "" : ", ") + ColumnDefinition(field);
  }
  const DdlWrite write =
      CreateTableWrite(TableDefinition{.table = table, .schema = schema, .metadata = metadata});
  try {
    backend_.ExecuteDdl("CREATE TABLE " + QualifiedName(table) + " (" + columns + ")",
                        write.metadata_statements, write.skip_query);
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Table " + table.project_id + ":" +
                                table.dataset_id + "." + table.table_id);
    }
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::CreateView(TableReference table, const json& definition,
                          const TableMetadata& metadata) {
  table.project_id = ResolveProject(table.project_id);
  WriteView(table, definition, metadata, /*replace=*/false);
}

void Emulator::UpdateTable(TableReference table,
                           const std::optional<std::vector<FieldSchema>>& schema,
                           const std::optional<json>& view,
                           const std::optional<TableMetadata>& metadata) {
  table.project_id = ResolveProject(table.project_id);
  const TableInfo info = GetTable(table, /*include_row_count=*/false);
  if (info.view_query.has_value()) {
    if (schema.has_value()) {
      throw ApiError::Invalid(
          "The emulator does not support changing the schema of a view; change its query instead");
    }
    if (view.has_value()) {
      // Every view the emulator keeps is GoogleSQL.
      json definition = {{"query", *info.view_query}, {"useLegacySql", false}};
      definition.update(*view);
      WriteView(table, definition, metadata.value_or(info.metadata), /*replace=*/true);
    } else if (metadata.has_value()) {
      Execute(ViewCommentStatement(
          table, {.query = *info.view_query, .schema = info.schema, .metadata = *metadata}));
    }
    return;
  }
  if (view.has_value()) {
    throw ApiError::Invalid("Table " + TableName(table) + " is not a view");
  }
  std::vector<std::string> statements;
  if (schema.has_value()) {
    statements = SchemaUpdateStatements(table, info.schema, *schema);
    std::ranges::move(ColumnCommentStatements(table, *schema), std::back_inserter(statements));
  }
  if (metadata.has_value()) {
    statements.push_back(TableCommentStatement(table, *metadata, schema.value_or(info.schema)));
  }
  if (statements.empty()) {
    return;
  }
  try {
    backend_.ExecuteDdl(statements.front(), {statements.begin() + 1, statements.end()}, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::WriteView(TableReference table, const json& definition,
                         const TableMetadata& metadata, bool replace) {
  table.project_id = ResolveProject(table.project_id);
  GetDataset(DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  if (definition.value("useLegacySql", true)) {
    throw ApiError::Invalid("The emulator does not support legacy SQL views");
  }
  if (definition.contains("userDefinedFunctionResources")) {
    throw ApiError::Invalid("The emulator does not support view userDefinedFunctionResources");
  }
  const std::string query = definition.value("query", "");
  if (query.empty()) {
    throw ApiError::Invalid("View query is required");
  }
  try {
    const TranslatedStatement translation =
        Translate(std::string(replace ? "CREATE OR REPLACE VIEW " : "CREATE VIEW ") +
                      googlesql::ToIdentifierLiteral(table.project_id + "." + table.dataset_id +
                                                     "." + table.table_id) +
                      " AS " + query,
                  {}, table.project_id, "");
    if (!translation.view.has_value()) {
      throw ApiError::Internal("CREATE VIEW was not translated to a view");
    }
    ViewDefinition view = *translation.view;
    view.metadata = metadata;
    backend_.ExecuteDdl(translation.sql, CreateViewWrite(view).metadata_statements, "");
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
  // Views the emulator did not create have no metadata for GetTable, but can still be dropped.
  if (ViewComment(backend_, table).has_value()) {
    Execute("DROP VIEW " + QualifiedName(table));
    return;
  }
  GetTable(table, false);
  Execute("DROP TABLE " + QualifiedName(table));
}

std::vector<Routine> Emulator::ListRoutines(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  std::vector<Routine> routines;
  for (const json& row : Execute(RoutinesQuery(dataset)).rows) {
    const RoutineReference reference{
        .project_id = dataset.project_id,
        .dataset_id = dataset.dataset_id,
        .routine_id = row["f"][0]["v"].get<std::string>(),
    };
    if (std::optional<Routine> routine = ParseRoutineComment(reference, row["f"][1]["v"])) {
      routines.push_back(*std::move(routine));
    }
  }
  return routines;
}

Routine Emulator::GetRoutine(RoutineReference routine) {
  routine.project_id = ResolveProject(routine.project_id);
  GetDataset(DatasetReference{.project_id = routine.project_id, .dataset_id = routine.dataset_id});
  const QueryResult result = Execute(RoutineQuery(routine));
  std::optional<Routine> found = result.rows.empty()
                                     ? std::nullopt
                                     : ParseRoutineComment(routine, result.rows.at(0)["f"][0]["v"]);
  if (!found.has_value()) {
    throw ApiError::NotFound("Not found: Routine " + routine.project_id + ":" + routine.dataset_id +
                             "." + routine.routine_id);
  }
  return *std::move(found);
}

Routine Emulator::WriteRoutine(Routine routine, bool update) {
  routine.reference.project_id = ResolveProject(routine.reference.project_id);
  const RoutineReference& reference = routine.reference;
  GetDataset({.project_id = reference.project_id, .dataset_id = reference.dataset_id});
  std::optional<Routine> previous;
  if (update) {
    previous = GetRoutine(reference);
  }
  try {
    const auto source = NewTableSource();
    CheckApiRoutineBody(routine, *source);
    std::string statement = RoutineStatement(routine);
    if (update) {
      statement.replace(0, std::string("CREATE").size(), "CREATE OR REPLACE");
    }
    const TranslatedStatement translation = Translate(statement, {}, reference.project_id, "");
    if (!translation.routine.has_value()) {
      throw ApiError::Invalid("Invalid routine definition");
    }
    const json& created = translation.routine->routine.resource;
    // Keep the API's body verbatim; the analyzer's code includes the newline we append to it.
    routine.resource["creationTime"] =
        previous.has_value() ? previous->resource.at("creationTime") : created.at("creationTime");
    routine.resource["lastModifiedTime"] = created.at("lastModifiedTime");
    backend_.ExecuteDdl(translation.sql, RoutineCommentStatements(routine), "");
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Routine " + reference.project_id + ":" +
                                reference.dataset_id + "." + reference.routine_id);
    }
    throw ApiError::Invalid(error.what());
  } catch (const std::exception& error) {
    throw ApiError::Invalid(error.what());
  }
  return routine;
}

void Emulator::DeleteRoutine(RoutineReference routine) {
  routine.project_id = ResolveProject(routine.project_id);
  GetRoutine(routine);
  Execute("DROP MACRO " + QualifiedName(routine));
}

QueryResult Emulator::ListTableData(TableReference table, int64_t start_index,
                                    int64_t max_results) {
  table.project_id = ResolveProject(table.project_id);
  if (GetTable(table).view_query) {
    throw ApiError::Invalid("Cannot read a view with tabledata.list; use a query instead");
  }
  return Execute("SELECT * FROM " + QualifiedName(table) + " LIMIT " + std::to_string(max_results) +
                 " OFFSET " + std::to_string(start_index));
}

std::vector<InsertError> Emulator::InsertTableData(TableReference table, const json& rows,
                                                   bool skip_invalid_rows,
                                                   bool ignore_unknown_values) {
  table.project_id = ResolveProject(table.project_id);
  if (!rows.is_array()) {
    throw ApiError::Invalid("rows must be an array");
  }
  const TableInfo info = GetTable(table, false);
  if (info.view_query) {
    throw ApiError::Invalid("Cannot insert into a view: " + TableName(table));
  }
  const std::vector<FieldSchema>& schema = info.schema;
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
        const bool known = std::ranges::any_of(
            schema, [&](const FieldSchema& field) { return field.name == it.key(); });
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
      statement += ')';
      statements.push_back(std::move(statement));
      indexes.push_back(i);
    } catch (const ApiError& error) {
      errors.push_back({.index = i, .message = error.what()});
    }
  }
  if (!skip_invalid_rows && !errors.empty()) {
    return errors;
  }
  try {
    for (const auto& [index, message] : backend_.InsertRows(statements, skip_invalid_rows)) {
      errors.push_back({.index = indexes.at(index), .message = message});
    }
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
  std::ranges::sort(errors,
                    [](const InsertError& a, const InsertError& b) { return a.index < b.index; });
  return errors;
}

}  // namespace bigquery_emulator_duckdb
