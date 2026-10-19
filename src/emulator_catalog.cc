#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
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
#include "src/external_query.h"
#include "src/field_schema.h"
#include "src/language_options.h"
#include "src/location.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/schema_sql.h"
#include "src/spanner_query.h"
#include "src/table_comments.h"
#include "src/table_metadata.h"
#include "src/table_path.h"
#include "src/table_source.h"
#include "src/translated_statement.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// Routine.definitionBody requires project-qualified references to other routines. Built-in
// namespaces such as NET and SAFE are still allowed.
void CheckApiRoutineBody(const Routine& routine, TableSource& source) {
  std::unique_ptr<googlesql::ParserOutput> parsed;
  const bool procedure = routine.resource.at("routineType") == "PROCEDURE";
  const bool tvf = routine.resource.at("routineType") == "TABLE_VALUED_FUNCTION";
  const std::string body = routine.resource.at("definitionBody").get<std::string>();
  const googlesql::ParserOptions options(GoogleSqlLanguageOptions());
  const auto status = procedure
                          ? googlesql::ParseScript(
                                body, options, {.mode = googlesql::ERROR_MESSAGE_ONE_LINE}, &parsed)
                      : tvf ? googlesql::ParseStatement(body, options, &parsed)
                            : googlesql::ParseExpression(body, options, &parsed);
  if (!status.ok()) {
    throw ApiError::Invalid(std::string(status.message()));
  }
  googlesql::TypeFactory types;
  BigQueryCatalog builtins(source, &types, "", "");
  std::vector<const googlesql::ASTNode*> nodes{
      procedure ? static_cast<const googlesql::ASTNode*>(parsed->script())
      : tvf     ? static_cast<const googlesql::ASTNode*>(parsed->statement())
                : parsed->expression(),
  };
  while (!nodes.empty()) {
    const auto* node = nodes.back();
    nodes.pop_back();
    if (const auto* call = node->GetAsOrNull<googlesql::ASTCallStatement>();
        call != nullptr &&
        SplitTablePath(call->procedure_name()->ToIdentifierVector()).size() != 3) {
      throw ApiError::Unsupported("API procedure bodies with CALL references without a project ID");
    }
    if (const auto* call = node->GetAsOrNull<googlesql::ASTFunctionCall>()) {
      std::vector<std::string> path = SplitTablePath(call->function()->ToIdentifierVector());
      if (!path.empty() && ToLowerAscii(path.front()) == "safe") {
        path.erase(path.begin());
      }
      const googlesql::Function* function = nullptr;
      if (path.size() == 2 && !builtins.FindFunction(path, &function).ok()) {
        throw ApiError::Unsupported(
            "API routine bodies with function references without a "
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
  const json object = json::parse(recorded.get<std::string>());
  DatasetMetadata metadata = DatasetMetadataFromJson(object);
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
                  const std::optional<DatasetReference>& dataset = std::nullopt) {
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
  for (const json& row : relations.rows) {
    const json& comment = row["f"][3]["v"];
    const auto expiration = CommentMetadata(comment).expiration_time;
    if (!expiration.has_value() || *expiration > now) {
      continue;
    }
    const TableReference table{
        .project_id = row["f"][0]["v"].get<std::string>(),
        .dataset_id = row["f"][1]["v"].get<std::string>(),
        .table_id = row["f"][2]["v"].get<std::string>(),
    };
    const bool view = row["f"][4]["v"] == "VIEW";
    // Recheck the comment in the drop's transaction: a concurrent update may have extended
    // the expiration, or another request may have replaced the table since the sweep.
    const std::string skip = std::format(
        "SELECT 1 WHERE NOT EXISTS (SELECT 1 FROM {} WHERE database_name = {}"
        " AND schema_name = {} AND {} = {} AND comment = {})",
        view ? "duckdb_views()" : "duckdb_tables()", QuoteLiteral(table.project_id),
        QuoteLiteral(table.dataset_id), view ? "view_name" : "table_name",
        QuoteLiteral(table.table_id), QuoteLiteral(comment.get<std::string>()));
    std::scoped_lock const lock(expiration_mutex);
    backend.ExecuteDdl(
        std::string(view ? "DROP VIEW IF EXISTS " : "DROP TABLE IF EXISTS ") + QualifiedName(table),
        {}, skip);
  }
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
// table does not exist, and ApiError when a column's DuckDB type is one the emulator does not
// support.
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
  schema = ApplyColumnComments(std::move(schema), comments);
  if (const std::optional<std::string> error = backend.UnreadableColumnError(QualifiedName(table));
      error.has_value()) {
    throw ApiError::Invalid(*error + "; change the column's type to read the table");
  }
  return schema;
}

// Queries see a GEOGRAPHY column as the STRING it is stored as, since the stored text is what the
// client wrote rather than the WKT that GEOGRAPHY values in queries hold; tables.get still
// reports the column as GEOGRAPHY.
void GeographyAsString(std::vector<FieldSchema>& schema) {
  for (FieldSchema& field : schema) {
    if (field.type == FieldType::kGeography) {
      field.type = FieldType::kString;
    }
    GeographyAsString(field.fields);
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
  return ParseDatasetMetadata(result.rows.at(0)["f"][1]["v"]).Location();
}

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
      GeographyAsString(schema);
      Use(project, dataset);
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
    DatasetMetadata metadata = ParseDatasetMetadata(result.rows.at(0)["f"][1]["v"]);
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
    std::ranges::transform(result.rows, std::back_inserter(counts), [](const json& row) {
      const json& value = row["f"][0]["v"];
      return std::pair{
          value.is_null() ? std::nullopt : std::optional(value.get<std::string>()),
          std::stoll(row["f"][1]["v"].get<std::string>()),
      };
    });
    return counts;
  }

  std::optional<ViewDescription> FindView(const std::string& project, const std::string& dataset,
                                          const std::string& table) override {
    const std::optional<json> comment = ViewComment(
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
    };
  }

  std::vector<Routine> ListRoutines(const DatasetReference& dataset) override {
    std::vector<Routine> routines;
    if (IsDuckDbSchema(dataset.dataset_id)) {
      return routines;
    }
    for (const json& row : backend_.Execute(RoutinesQuery(dataset)).rows) {
      if (auto routine = ParseRoutineComment(
              {
                  .project_id = dataset.project_id,
                  .dataset_id = dataset.dataset_id,
                  .routine_id = row["f"][0]["v"].get<std::string>(),
              },
              row["f"][1]["v"])) {
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
    std::optional<Routine> found = ParseRoutineComment(routine, result.rows.at(0)["f"][0]["v"]);
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
    if (const std::optional<std::string> location = ConnectionLocation(key); location.has_value()) {
      ReferenceLocation("Connection " + key, *location);
    }
    const bool spanner = connection->second.starts_with("spanner://");
    absl::StatusOr<std::vector<FieldSchema>> columns =
        [&]() -> absl::StatusOr<std::vector<FieldSchema>> {
      if (spanner) {
        return DescribeSpannerQuery(connection->second.substr(10), query, options);
      }
      const auto decimal_type = DecimalColumnType(options);
      if (!decimal_type.ok()) {
        return decimal_type.status();
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

// The DuckDB expression for `value`, a value of `field` in a tabledata.insertAll row. DuckDB's
// casts round halfway values away from zero, so the values of ROUND_HALF_EVEN fields are read
// unrounded and rounded to even, as BigQuery rounds the rows it streams.
std::string RoundedInsertValue(const json& value, const FieldSchema& field,
                               bool ignore_unknown_values) {
  const FieldSchema unrounded = WithoutRoundingParameters(field);
  const std::optional<std::string> rounded = TranslateAppendedValue(
      InsertValue(value, unrounded, ignore_unknown_values), unrounded, field);
  if (!rounded.has_value()) {
    throw ApiError::Unsupported("tabledata.insertAll into ROUND_HALF_EVEN columns");
  }
  return *rounded;
}

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

DatasetMetadata Emulator::CreatedInDataset(Backend& backend,
                                           const TranslatedStatement& translation) {
  std::optional<TableReference> table;
  if (translation.table.has_value() && !translation.table->temporary) {
    table = translation.table->table;
  } else if (translation.view.has_value()) {
    table = translation.view->table;
  }
  if (!table.has_value()) {
    return {};
  }
  const QueryResult result =
      backend.Execute(DatasetEntriesQuery(table->project_id, table->dataset_id));
  return result.rows.empty() ? DatasetMetadata{}
                             : ParseDatasetMetadata(result.rows.at(0)["f"][1]["v"]);
}

std::optional<std::string> Emulator::DatasetLocation(Backend& backend,
                                                     const DatasetReference& dataset) {
  return LocationOf(backend, dataset);
}

void Emulator::UseTargets(Backend& backend, const TranslatedStatement& translation,
                          JobLocation& location) {
  const auto use = [&](const DatasetReference& dataset) {
    if (const std::optional<std::string> found = DatasetLocation(backend, dataset)) {
      location.Reference("Dataset " + dataset.project_id + ":" + dataset.dataset_id, *found);
    }
  };
  if (const auto& definition = translation.dataset) {
    // The location CREATE SCHEMA names does not settle the job's, as BigQuery answers, even for a
    // dataset that IF NOT EXISTS keeps.
    const std::string& named = definition->metadata.location;
    if (!named.empty() && named != location.Resolve()) {
      throw ApiError::InvalidQuery("Location specified in query " + named +
                                   " is not consistent with current execution region " +
                                   location.Resolve());
    }
  } else if (const auto& target = translation.ddl_target_dataset) {
    use(*target);
  }
  if (const auto& target = translation.ddl_target_table) {
    use({.project_id = target->project_id, .dataset_id = target->dataset_id});
  }
  if (const auto& target = translation.ddl_target_routine) {
    use({.project_id = target->project_id, .dataset_id = target->dataset_id});
  }
}

std::unique_ptr<TableSource> Emulator::NewTableSource(Backend* session, bool expire,
                                                      JobLocation* location) {
  Backend& backend = session != nullptr ? *session : backend_;
  if (expire) {
    ExpireTables(backend, expiration_mutex_);
  }
  return std::make_unique<DuckDbTableSource>(backend, connections_, project_ids_, location);
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
  const std::vector<std::string> statements =
      CreateDatasetWrite({.dataset = dataset, .metadata = metadata}).metadata_statements;
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
    if (previous->resource.at("routineType") != routine.resource.at("routineType")) {
      throw ApiError::Unsupported("changing a routine's type");
    }
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

std::vector<InsertError> Emulator::WriteStorageRows(TableReference table,
                                                    const std::vector<FieldSchema>& schema,
                                                    const json& rows, int64_t storage_id,
                                                    bool validate_only) {
  table.project_id = ResolveProject(table.project_id);
  if (GetTable(table, false).view_query) {
    throw ApiError::Invalid("Cannot write to a view: " + TableName(table));
  }
  try {
    const auto session = backend_.NewSession();
    session->Transaction("BEGIN TRANSACTION");
    const QueryResult identity =
        session->Execute(std::format("SELECT table_oid FROM duckdb_tables() WHERE database_name = "
                                     "{} AND schema_name = {} AND table_name = {}",
                                     QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id),
                                     QuoteLiteral(table.table_id)));
    if (identity.rows.empty() || std::stoll(FirstColumnStrings(identity).at(0)) != storage_id) {
      throw ApiError::NotFound("The Storage Write destination table was dropped or replaced");
    }
    if (SchemaToJson(TableSchema(*session, table)) != SchemaToJson(schema)) {
      throw ApiError::Invalid("Storage Write does not support destination schema changes");
    }
    std::vector<std::string> statements;
    for (size_t index = 0; index < rows.size(); ++index) {
      const auto& row = rows.at(index);
      std::vector<std::string> columns;
      std::vector<std::string> values;
      for (const auto& field : schema) {
        columns.push_back(QuoteIdentifier(field.name));
        const auto value = row.find(field.name);
        try {
          values.push_back(InsertValue(value == row.end() ? json(nullptr) : *value, field, false));
        } catch (const ApiError& error) {
          return {{.index = index, .message = error.what()}};
        }
      }
      statements.push_back(std::format("INSERT INTO {} ({}) VALUES ({})", QualifiedName(table),
                                       Join(columns, ", "), Join(values, ", ")));
    }
    for (size_t index = 0; index < statements.size(); ++index) {
      try {
        session->Execute(statements.at(index));
      } catch (const BackendError& error) {
        return {{.index = index, .message = error.what()}};
      }
    }
    session->Transaction(validate_only ? "ROLLBACK" : "COMMIT");
    return {};
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
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
      std::vector<std::string> columns;
      std::vector<std::string> literals;
      for (const FieldSchema& field : schema) {
        columns.push_back(QuoteIdentifier(field.name));
        const auto it = values.find(field.name);
        literals.push_back(
            it == values.end() ? "DEFAULT" : RoundedInsertValue(*it, field, ignore_unknown_values));
      }
      std::string statement = "INSERT INTO ";
      statement += QualifiedName(table);
      statement += " (";
      statement += Join(columns, ", ");
      statement += ") VALUES (";
      statement += Join(literals, ", ");
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
