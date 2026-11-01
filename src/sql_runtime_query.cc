#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/analyzer.h"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/builtin_function_options.h"
#include "src/column_default.h"
#include "src/ddl_write.h"
#include "src/default_dataset.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/duckdb_table_source.h"
#include "src/field_schema.h"
#include "src/language_options.h"
#include "src/location.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/result_value.h"
#include "src/routine_catalog.h"
#include "src/schema_sql.h"
#include "src/script.h"
#include "src/sql_runtime.h"
#include "src/statement_resources.h"
#include "src/storage_snapshot.h"
#include "src/table_source.h"
#include "src/translated_statement.h"
#include "src/translator.h"
#include "src/translator/literal.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

void RoutineError(const TranslatedStatement& translation, const BackendError& error) {
  const std::optional<RoutineReference>& routine = translation.ddl_target_routine;
  if (!routine.has_value()) {
    return;
  }
  const std::string name =
      routine->project_id + ":" + routine->dataset_id + "." + routine->routine_id;
  const std::string_view message = error.what();
  if (message.find("Schema with name") != std::string_view::npos) {
    throw ApiError::NotFound("Not found: Dataset " + routine->project_id + ":" +
                             routine->dataset_id);
  }
  if (message.find("already exists") != std::string_view::npos) {
    throw ApiError::Duplicate("Already Exists: Function " + name);
  }
  if (message.find("does not exist") != std::string_view::npos) {
    throw ApiError::NotFound("Not found: Function " + name);
  }
}

// Checks stored definitions in their own context, without the job's defaults or local objects.
void CheckDefinitions(const TranslatedStatement& translation, RoutineCatalog& catalog) {
  if (translation.routine) {
    if (const absl::Status status = catalog.CheckRoutine(translation.routine->routine);
        !status.ok()) {
      throw ApiError::InvalidQuery(std::string(status.message()));
    }
  }
  if (translation.view) {
    if (const absl::Status status =
            catalog.CheckView(translation.view->table, translation.view->query);
        !status.ok()) {
      throw ApiError::InvalidQuery(std::string(status.message()));
    }
  }
}

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
bool SameWireEncoding(FieldType a, FieldType b) {
  static const auto* const kGroups = new std::vector<std::set<FieldType>>{
      {FieldType::kInteger, FieldType::kFloat, FieldType::kNumeric, FieldType::kBigNumeric},
      {FieldType::kString, FieldType::kJson, FieldType::kGeography},
  };
  if (a == b) {
    return true;
  }
  return std::ranges::any_of(*kGroups, [&](const std::set<FieldType>& group) {
    return group.contains(a) && group.contains(b);
  });
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
    FieldSchema& field = duckdb_schema.at(i);
    const FieldSchema& resolved = resolved_schema.at(i);
    field.name = resolved.name;
    field.collation = resolved.collation;
    if (field.mode != resolved.mode) {
      continue;
    }
    if (field.type == FieldType::kRecord && resolved.type == FieldType::kRecord) {
      field.fields = ReconcileSchema(std::move(field.fields), resolved.fields);
    } else if (SameWireEncoding(field.type, resolved.type)) {
      field.type = resolved.type;
    }
  }
  return duckdb_schema;
}

// A query without a destination table returns each duplicate column after the first renamed:
// SELECT 1 AS a, 2 AS a returns a and a_1.
// third_party/bigquery_docs/reference/standard-sql/query-syntax.md
// BigQuery renames the second of two columns whose names differ at most in case, keeping its
// spelling, as `A_1` for `SELECT 1 AS a, 2 AS A`. The names it takes for a name repeated more than
// twice or a renamed column that would collide with another are not documented, so those are
// unsupported.
std::vector<FieldSchema> UniqueResultNames(std::vector<FieldSchema> schema) {
  const auto folded = [](std::string name) {
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
    return name;
  };
  std::map<std::string, std::vector<size_t>> columns;
  for (size_t i = 0; i < schema.size(); ++i) {
    columns[folded(schema.at(i).name)].push_back(i);
  }
  for (const auto& [name, indexes] : columns) {
    // Only GoogleSQL can return anonymous value-table fields; keep their empty names.
    if (name.empty() || indexes.size() == 1) {
      continue;
    }
    FieldSchema& duplicate = schema.at(indexes.at(1));
    if (indexes.size() > 2 || columns.contains(name + "_1")) {
      throw ApiError::UnsupportedQuery("these duplicate column names in a query result: " +
                                       duplicate.name);
    }
    duplicate.name += "_1";
  }
  return schema;
}

// The script that `query` is, or null when it is a single statement. BigQuery parses every query as
// a script, so a query that does not parse as one fails with the script parser's error, as the
// analyzer reports a statement's, rather than with the error of a single statement it is not.
std::unique_ptr<googlesql::ParserOutput> ParseScript(const std::string& query) {
  std::unique_ptr<googlesql::ParserOutput> output;
  if (const absl::Status status =
          googlesql::ParseScript(query, googlesql::ParserOptions(GoogleSqlLanguageOptions()),
                                 {.mode = googlesql::ERROR_MESSAGE_MULTI_LINE_WITH_CARET}, &output);
      !status.ok()) {
    throw std::runtime_error(status.ToString());
  }
  // A TEMP function needs a job-local catalog even when it is the only statement, and EXECUTE
  // IMMEDIATE, which GoogleSQL parses as a SQL statement, runs as a script.
  const auto& statements = output->script()->statement_list();
  if (statements.size() == 1 && statements.at(0)->IsSqlStatement() &&
      statements.at(0)->node_kind() != googlesql::AST_EXECUTE_IMMEDIATE_STATEMENT &&
      statements.at(0)->node_kind() != googlesql::AST_CALL_STATEMENT) {
    const auto* create = statements.at(0)->GetAsOrNull<googlesql::ASTCreateFunctionStatement>();
    if (create == nullptr || !create->is_temp()) {
      return nullptr;
    }
  }
  return statements.empty() ? nullptr : std::move(output);
}

// TEMP declarations followed by one SELECT are a SELECT job; other statement lists are SCRIPT.
std::string ScriptStatementType(const googlesql::ParserOutput& script) {
  const auto& statements = script.script()->statement_list();
  if (statements.size() == 1 && statements.at(0)->node_kind() == googlesql::AST_CALL_STATEMENT) {
    return "CALL";
  }
  if (statements.size() == 1 &&
      statements.at(0)->node_kind() == googlesql::AST_CREATE_FUNCTION_STATEMENT) {
    return "CREATE_FUNCTION";
  }
  if (statements.empty() || statements.back()->node_kind() != googlesql::AST_QUERY_STATEMENT) {
    return kScriptStatementType;
  }
  const bool query =
      std::all_of(statements.begin(), statements.end() - 1, [](const auto* statement) {
        const auto* create =
            statement->template GetAsOrNull<googlesql::ASTCreateFunctionStatement>();
        return create != nullptr && create->is_temp();
      });
  return query ? "SELECT" : kScriptStatementType;
}

// A field of a Storage Read projection, with only the selected fields of a RECORD.
struct Projection {
  FieldSchema field;
  bool whole = false;
  std::vector<Projection> children;
};

// The fields selected_fields names, in the table's field order, including inside RECORDs.
std::vector<Projection> Select(const std::vector<FieldSchema>& schema,
                               const std::vector<std::string>& paths) {
  std::vector<Projection> result;
  std::vector<bool> matched(paths.size(), false);
  for (const auto& field : schema) {
    bool whole = paths.empty();
    std::vector<std::string> children;
    for (size_t j = 0; j < paths.size(); ++j) {
      const auto dot = paths.at(j).find('.');
      if (ToLowerAscii(paths.at(j).substr(0, dot)) == ToLowerAscii(field.name)) {
        matched.at(j) = true;
        if (dot == std::string::npos) {
          whole = true;
        } else {
          children.push_back(paths.at(j).substr(dot + 1));
        }
      }
    }
    if (!whole && children.empty()) {
      continue;
    }
    Projection projection{.field = field, .whole = whole, .children = {}};
    if (!children.empty() && field.type != FieldType::kRecord) {
      throw ApiError::Invalid("Cannot select a nested field of " + field.name);
    }
    if (field.type == FieldType::kRecord) {
      if (whole && !children.empty()) {
        (void)Select(field.fields, children);
      }
      projection.children = Select(field.fields, whole ? std::vector<std::string>{} : children);
      projection.field.fields.clear();
      for (const auto& child : projection.children) {
        projection.field.fields.push_back(child.field);
      }
    }
    result.push_back(std::move(projection));
  }
  for (size_t j = 0; j < paths.size(); ++j) {
    if (!matched.at(j)) {
      throw ApiError::Invalid("Unknown selected field: " + paths.at(j));
    }
  }
  return result;
}

// The GoogleSQL expression of `projection` of `value`. A NULL RECORD stays NULL, and the
// elements of a REPEATED one keep their order.
std::string ProjectionSql(const Projection& projection, const std::string& value, int depth) {
  if (projection.whole) {
    return value;
  }
  const auto fields = [&](const std::string& record) {
    std::vector<std::string> items;
    for (const auto& child : projection.children) {
      const std::string name = googlesql::ToIdentifierLiteral(child.field.name);
      std::string path = record;
      path += "." + name;
      std::string item = ProjectionSql(child, path, depth + 1);
      item += " AS " + name;
      items.push_back(std::move(item));
    }
    return absl::StrJoin(items, ", ");
  };
  if (projection.field.mode == FieldMode::kRepeated) {
    const std::string element = "_bq_element_" + std::to_string(depth);
    const std::string offset = "_bq_offset_" + std::to_string(depth);
    return "ARRAY(SELECT AS STRUCT " + fields(element) + " FROM UNNEST(" + value + ") AS " +
           element + " WITH OFFSET AS " + offset + " ORDER BY " + offset + ")";
  }
  return "IF(" + value + " IS NULL, NULL, STRUCT(" + fields(value) + "))";
}

}  // namespace

// Runs the statements of a script as parts of one job, in `location`.
class SqlRuntime::ScriptHostImpl : public ScriptHost {
 public:
  ScriptHostImpl(SqlRuntime& emulator, JobLocation& location)
      : emulator_(emulator), location_(location) {}

  Backend& backend() override { return emulator_.backend_; }
  [[nodiscard]] bool has_session_user() const override { return emulator_.has_session_user_; }
  [[nodiscard]] const ExcludedFunctionIds& excluded_function_ids() const override {
    return emulator_.excluded_function_ids_;
  }
  [[nodiscard]] FunctionDeclarations function_declarations() const override {
    return emulator_.declarations_;
  }
  [[nodiscard]] const function_catalog::Catalog& functions() const override {
    return emulator_.functions_;
  }
  std::unique_ptr<TableSource> NewTableSource(Backend* session, bool expire) override {
    return emulator_.NewTableSource(session, expire, &location_);
  }
  QueryResult RunStatement(const TranslatedStatement& translation,
                           const std::vector<std::string>& setup, bool null_arrays,
                           Backend* session) override {
    return emulator_.RunStatement(translation, setup, null_arrays, session, &location_);
  }

 private:
  SqlRuntime& emulator_;
  JobLocation& location_;
};

std::unique_ptr<TableSource> SqlRuntime::NewTableSource(Backend* session, bool expire,
                                                        JobLocation* location) {
  Backend& backend = session != nullptr ? *session : backend_;
  if (expire) {
    ExpireTables(backend, expiration_mutex_);
  }
  return NewDuckDbTableSource(backend, project_ids_, location,
                              external_tables_ ? external_tables_(backend_) : nullptr);
}

QueryResult SqlRuntime::ExecuteDuckDb(const std::string& sql) { return Execute(sql); }

QueryResult SqlRuntime::Execute(const std::string& sql, const std::vector<std::string>& setup,
                                bool null_arrays) {
  try {
    return backend_.Execute(sql, setup, null_arrays);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

QueryResult SqlRuntime::Prepare(const std::string& sql, const std::vector<std::string>& setup) {
  try {
    return backend_.Prepare(sql, setup);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

std::shared_ptr<const StorageSnapshot> SqlRuntime::ReadStorageTable(
    TableReference table, const std::vector<std::string>& selected_fields,
    const std::string& restriction) {
  table.project_id = ResolveProject(table.project_id);
  const TableInfo info = GetTable(table, false);
  if (info.metadata.external_data) {
    throw ApiError::Unsupported("Storage Read of external tables");
  }
  if (info.view_query) {
    throw ApiError::Invalid("Storage Read does not support logical views");
  }
  // SELECT * preserves the table's field order and its stored modes and logical types.
  std::vector<std::string> columns;
  std::vector<FieldSchema> schema;
  for (const auto& projection : Select(info.schema, selected_fields)) {
    const std::string name = googlesql::ToIdentifierLiteral(projection.field.name);
    columns.push_back(ProjectionSql(projection, name, 0) + " AS " + name);
    schema.push_back(projection.field);
  }
  std::string query = "SELECT " +
                      (selected_fields.empty() ? std::string("*") : absl::StrJoin(columns, ", ")) +
                      " FROM " +
                      googlesql::ToIdentifierLiteral(table.project_id + "." + table.dataset_id +
                                                     "." + table.table_id);
  if (!restriction.empty()) {
    if (restriction.size() > size_t{1024} * 1024) {
      throw ApiError::Invalid("row_restriction exceeds 1 MB");
    }
    std::unique_ptr<googlesql::ParserOutput> parsed;
    const auto status = googlesql::ParseExpression(
        restriction, googlesql::ParserOptions(GoogleSqlLanguageOptions()), &parsed);
    if (!status.ok()) {
      throw ApiError::Invalid(std::string(status.message()));
    }
    std::vector<const googlesql::ASTNode*> nodes{parsed->expression()};
    while (!nodes.empty()) {
      const auto* node = nodes.back();
      nodes.pop_back();
      if (node->GetAsOrNull<googlesql::ASTExpressionSubquery>() != nullptr ||
          node->GetAsOrNull<googlesql::ASTQuery>() != nullptr) {
        throw ApiError::Invalid("Storage Read does not support subqueries in row_restriction");
      }
      for (int i = 0; i < node->num_children(); ++i) {
        nodes.push_back(node->child(i));
      }
    }
    query += " WHERE (" + restriction + ")";
  }
  const TranslatedStatement translated = Translate(query, {}, table.project_id, table.dataset_id);
  std::shared_ptr<const StorageSnapshot> snapshot;
  try {
    snapshot = backend_.ExecuteStorageRead(translated.sql, std::move(schema), translated.resources);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
  const TableInfo after = GetTable(table, false);
  if (after.storage_id != info.storage_id ||
      SchemaToJson(after.schema) != SchemaToJson(info.schema)) {
    throw ApiError::Invalid("Table changed while creating the Storage Read snapshot");
  }
  return snapshot;
}

TranslatedStatement SqlRuntime::Translate(
    const std::string& query, const QueryParameters& parameters, const std::string& default_project,
    const std::string& default_dataset, JobLocation* location,
    const googlesql::SystemVariableValuesMap* system_variables, bool stores_result, bool dry_run) {
  AnalyzerSettings settings{
      .default_project = default_project,
      .default_dataset = default_dataset,
      .system_variables = system_variables,
      .restrict_struct_results = functions_.restrictions().restrict_struct_results,
  };
  googlesql::TypeFactory type_factory;
  for (const FieldSchema& field : parameters.named_types()) {
    settings.named_parameters.emplace_back(field.name, ParameterType(field, type_factory));
  }
  for (const FieldSchema& field : parameters.positional_types()) {
    settings.positional_parameters.push_back(ParameterType(field, type_factory));
  }
  const auto source = NewTableSource(nullptr, true, location);
  RoutineCatalog catalog(*source, &type_factory, settings.default_project, settings.default_dataset,
                         functions_.timestamp_precision(), nullptr, excluded_function_ids_,
                         declarations_);
  const AnalyzerResult analyzed =
      AnalyzeGoogleSql(query, catalog, type_factory, functions_.timestamp_precision(), settings);
  std::string unsupported;
  std::optional<TranslatedStatement> translated = TranslateStatement(
      functions_, analyzed.statement(), parameters,
      DefaultDataset{
          .project = settings.default_project,
          .dataset = settings.default_dataset,
          .temporary = nullptr,
          .source = source.get(),
          .resources = std::make_shared<StatementResources>(
              dry_run ? StatementResources::Mode::kDryRun : StatementResources::Mode::kExecute),
          .has_session_user = has_session_user_,
          .stores_result = stores_result,
          .evaluate =
              [this](const std::string& sql, const googlesql::Type* type) {
                return ResultValue(Execute(sql, {}, true), type);
              },
      },
      &unsupported, system_variables);
  if (!translated.has_value()) {
    throw ApiError::UnsupportedQuery(unsupported);
  }
  CheckDefinitions(*translated, catalog);
  return *std::move(translated);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a query, then its defaults.
void SqlRuntime::InferLocation(const std::string& query, const std::string& default_project,
                               const std::string& default_dataset, JobLocation& location) {
  googlesql::TableNamesSet names;
  if (!googlesql::ExtractTableNamesFromScript(
           query, googlesql::AnalyzerOptions(GoogleSqlLanguageOptions()), &names)
           .ok()) {
    return;
  }
  std::set<std::string> locations;
  for (const std::vector<std::string>& name : names) {
    // A quoted name such as `project.dataset.table` is one identifier of the path.
    std::vector<std::string> parts;
    for (const std::string& identifier : name) {
      const std::vector<std::string> split = absl::StrSplit(identifier, '.');
      parts.insert(parts.end(), split.begin(), split.end());
    }
    std::optional<std::string> region;
    for (const std::string& part : parts) {
      if (!region.has_value()) {
        region = RegionQualifierLocation(part);
      }
    }
    if (region.has_value()) {
      locations.insert(*std::move(region));
      continue;
    }
    // The dataset of an INFORMATION_SCHEMA view, or that of a table. SCHEMATA without a region
    // qualifier is the US region's.
    const auto schema = std::ranges::find_if(
        parts, [](const std::string& part) { return ToUpperAscii(part) == "INFORMATION_SCHEMA"; });
    if (schema != parts.end() && schema + 1 != parts.end() &&
        ToUpperAscii(*(schema + 1)) == "SCHEMATA") {
      locations.insert(kDefaultLocation);
      continue;
    }
    if (schema != parts.end()) {
      parts.erase(schema, parts.end());
    } else if (!parts.empty()) {
      parts.pop_back();
    }
    DatasetReference dataset{.project_id = default_project, .dataset_id = default_dataset};
    if (!parts.empty()) {
      dataset.dataset_id = parts.back();
      if (parts.size() > 1) {
        dataset.project_id = absl::StrJoin(parts.begin(), parts.end() - 1, ".");
      }
    }
    if (dataset.dataset_id.empty()) {
      continue;
    }
    try {
      dataset.project_id = ResolveProject(dataset.project_id);
    } catch (const ApiError&) {
      continue;
    }
    if (const std::optional<std::string> found = DatasetLocation(backend_, dataset)) {
      locations.insert(*found);
    }
  }
  // Names of several locations leave the job in the US, as BigQuery answers.
  if (!locations.empty()) {
    location.Infer(locations.size() == 1 ? *locations.begin() : std::string(kDefaultLocation));
  }
}

std::vector<std::string> SqlRuntime::ColumnDefaultStatements(
    const TableReference& table, const std::vector<FieldSchema>& schema) {
  std::vector<std::string> statements;
  for (const FieldSchema& field : schema) {
    if (field.default_value_expression.empty()) {
      continue;
    }
    googlesql::TypeFactory type_factory;
    const absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
    if (!type.ok()) {
      throw ApiError::Invalid(std::string(type.status().message()));
    }
    const AnalyzerSettings settings{.default_project = table.project_id};
    // A default cannot read tables, so they need not be expired first.
    const auto source = NewTableSource(nullptr, /*expire=*/false);
    RoutineCatalog catalog(*source, &type_factory, table.project_id, "",
                           functions_.timestamp_precision(), nullptr, excluded_function_ids_,
                           declarations_);
    const absl::StatusOr<AnalyzerResult> analyzed =
        AnalyzeScriptExpression(field.default_value_expression, nullptr, *type, catalog,
                                type_factory, functions_.timestamp_precision(), settings);
    if (!analyzed.ok()) {
      throw ApiError::Invalid("Invalid default value for field " + field.name + ": " +
                              std::string(analyzed.status().message()));
    }
    const auto* literal = analyzed->literal_value();
    ValidateColumnDefault(field, literal != nullptr ? std::optional(*literal) : std::nullopt);
    if (literal != nullptr && RoundsToEven(field)) {
      const auto sql = translator::Literal(RoundedColumnDefault(field, *literal),
                                           functions_.timestamp_precision());
      if (!sql) {
        throw ApiError::Unsupported("default value of field " + field.name);
      }
      statements.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} SET DEFAULT {}",
                                       QualifiedName(table), QuoteIdentifier(field.name), *sql));
      continue;
    }
    std::string unsupported;
    const std::optional<std::string> query =
        TranslateExpression(functions_, analyzed->expression(), {},
                            DefaultDataset{
                                .project = table.project_id,
                                .dataset = {},
                                .temporary = nullptr,
                                .has_session_user = has_session_user_,
                            },
                            &unsupported, nullptr);
    if (!query.has_value()) {
      throw ApiError::Unsupported(unsupported + " in the default value of field " + field.name);
    }
    // TranslateExpression returns the expression as a query, "SELECT <expression>".
    statements.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} SET DEFAULT {}",
                                     QualifiedName(table), QuoteIdentifier(field.name),
                                     query->substr(std::string_view("SELECT ").size())));
  }
  return statements;
}

QueryResult SqlRuntime::ExecuteQuery(QueryRequest request, QueryJob& query, JobLocation& location) {
  request.project_id = ResolveProject(request.project_id);
  if (request.destination_table) {
    auto& project = request.destination_table->project_id;
    project = ResolveProject(project.empty() ? request.project_id : project);
  }
  std::vector<std::string> setup;
  AnalyzerSettings settings{
      .default_project = request.project_id,
  };
  if (request.default_dataset.has_value()) {
    const std::string dataset_project = ResolveProject(request.default_dataset->project_id.empty()
                                                           ? request.project_id
                                                           : request.default_dataset->project_id);
    setup.push_back("USE " + QualifiedName(DatasetReference{
                                 .project_id = dataset_project,
                                 .dataset_id = request.default_dataset->dataset_id,
                             }));
    settings.default_project = dataset_project;
    settings.default_dataset = request.default_dataset->dataset_id;
  } else {
    setup.push_back("USE " + QuoteIdentifier(request.project_id));
  }

  query.destination_table = request.destination_table;
  InferLocation(request.query, settings.default_project, settings.default_dataset, location);
  if (request.destination_table.has_value()) {
    const DatasetReference dataset{
        .project_id = request.destination_table->project_id,
        .dataset_id = request.destination_table->dataset_id,
    };
    if (const std::optional<std::string> found = DatasetLocation(backend_, dataset)) {
      location.Reference("Dataset " + dataset.project_id + ":" + dataset.dataset_id, *found);
    }
  }

  if (const auto script = ParseScript(request.query)) {
    query.statement_type = ScriptStatementType(*script);
    if (request.dry_run) {
      throw ApiError::UnsupportedQuery("dry runs of multi-statement queries");
    }
    if (request.destination_table.has_value()) {
      throw ApiError::Invalid("Cannot set destination table in multi-statement queries");
    }
    ScriptHostImpl host(*this, location);
    return RunScript(host, request.query, *script, request.parameters, settings.default_project,
                     settings.default_dataset, setup, request.project_id, next_script_number_++);
  }
  const googlesql::SystemVariableValuesMap variables{
      {{"project_id"}, googlesql::Value::String(request.project_id)},
  };
  const TranslatedStatement translation = Translate(
      request.query, request.parameters, settings.default_project, settings.default_dataset,
      &location, &variables, request.destination_table.has_value(), request.dry_run);
  query.statement_type = translation.statement_type;
  query.ddl_target_table = translation.ddl_target_table;
  query.ddl_target_dataset = translation.ddl_target_dataset;
  query.ddl_target_routine = translation.ddl_target_routine;
  if (!request.dry_run && !request.destination_table.has_value()) {
    return RunStatement(translation, setup, request.null_arrays, nullptr, &location);
  }
  CheckDdlTarget(translation);
  UseTargets(backend_, translation, location);
  if (request.destination_table.has_value() && !translation.result_schema.has_value()) {
    throw ApiError::Invalid("Cannot set destination table in jobs with DML/DDL statements");
  }
  QueryResult result;
  if (!request.dry_run) {
    // A destination table reports the mode of each field, as a query result does.
    std::vector<FieldSchema> written = *translation.result_schema;
    SetModesGiven(written);
    result =
        WriteDestination(request.project_id, *request.destination_table, request.create_disposition,
                         request.write_disposition, translation.sql, written, setup);
    result.rows.KeepResources(translation.resources);
  } else if (!AlterationStatements(translation).has_value()) {
    // A dry run checks an alteration against the table or dataset without running it.
    std::unique_lock<std::mutex> graph_lock(graph_mutex_, std::defer_lock);
    if (translation.property_graph || translation.reads_property_graph) {
      graph_lock.lock();
    }
    result = Prepare(translation.sql, setup);
  }
  if (translation.result_schema.has_value()) {
    result.schema = ReconcileSchema(std::move(result.schema),
                                    request.destination_table.has_value()
                                        ? *translation.result_schema
                                        : UniqueResultNames(*translation.result_schema));
  }
  return result;
}

QueryResult SqlRuntime::RunStatement(const TranslatedStatement& translation,
                                     const std::vector<std::string>& setup, bool null_arrays,
                                     Backend* session, JobLocation* location) {
  Backend& backend = session != nullptr ? *session : backend_;
  std::unique_lock<std::recursive_mutex> expiration_lock(expiration_mutex_, std::defer_lock);
  // CREATE responses read the stored schema after committing the DDL. Keep another
  // request's expiration sweep from dropping the relation before that read completes.
  if (translation.statement_type == "DROP_SCHEMA" ||
      (translation.table && translation.table->as_select) ||
      (translation.view && !translation.view->materialized)) {
    expiration_lock.lock();
  }
  std::unique_lock<std::mutex> graph_lock(graph_mutex_, std::defer_lock);
  if (translation.property_graph || translation.reads_property_graph) {
    graph_lock.lock();
  }
  CheckDdlTarget(translation);
  CheckRelationTarget(backend, translation);
  if (const auto& table = translation.table;
      table && table->metadata.external_data && table->schema.empty()) {
    // IF NOT EXISTS keeps what is there without reading the sources.
    if (table->if_not_exists &&
        (RelationComment(backend, "duckdb_tables()", "table_name", table->table) ||
         ViewComment(backend, table->table))) {
      return {};
    }
    TranslatedStatement detected = translation;
    // tables.get reports a table that CREATE EXTERNAL TABLE detects the schema of as autodetected.
    detected.table->metadata.external_data->autodetect = true;
    detected.table->schema = DetectExternalSchema(*detected.table->metadata.external_data);
    const auto columns =
        ColumnDefinitions(detected.table->schema, functions_.timestamp_precision());
    if (!columns) {
      throw ApiError::Unsupported("the detected schema of external table " +
                                  TableName(table->table));
    }
    detected.sql += " (" + *columns + ")";
    return RunStatement(detected, setup, null_arrays, session, location);
  }
  if (translation.ddl_target_routine) {
    const auto source = NewTableSource(&backend, false);
    const auto previous = source->FindRoutine(*translation.ddl_target_routine);
    const std::string type = translation.statement_type == "CREATE_PROCEDURE" ||
                                     translation.statement_type == "DROP_PROCEDURE"
                                 ? "PROCEDURE"
                             : translation.statement_type == "CREATE_TABLE_FUNCTION" ||
                                     translation.statement_type == "DROP_TABLE_FUNCTION"
                                 ? "TABLE_VALUED_FUNCTION"
                                 : "SCALAR_FUNCTION";
    if (previous && previous->resource.at("routineType") != type) {
      throw ApiError::Unsupported("replacing or dropping a routine of a different type");
    }
  }
  JobLocation unchecked;
  if (location == nullptr) {
    location = &unchecked;
  }
  UseTargets(backend, translation, *location);
  std::optional<std::vector<FieldSchema>> schema;
  if (translation.result_schema.has_value()) {
    schema = UniqueResultNames(*translation.result_schema);
  }
  QueryResult result;
  if (translation.property_graph) {
    const auto& graph = *translation.property_graph;
    if (!DatasetLocation(backend,
                         {.project_id = graph.project_id, .dataset_id = graph.dataset_id})) {
      throw ApiError::NotFound("Not found: Dataset " + graph.project_id + ":" + graph.dataset_id);
    }
    const auto previous = backend.Execute(PropertyGraphQuery(graph));
    const bool creating = translation.property_graph_definition.has_value();
    const bool exists = !previous.rows.empty();
    if (creating == exists && translation.skips_property_graph) {
      return result;
    }
    if (creating && exists && !translation.replaces) {
      throw ApiError::Duplicate("Property graph already exists");
    }
    if (!creating && !exists) {
      throw ApiError::NotFound("Property graph not found");
    }
    try {
      backend.Execute(translation.sql, setup);
    } catch (const BackendError& error) {
      if (std::string_view(error.what()).find("Incomplete edge") != std::string_view::npos) {
        throw ApiError::UnsupportedQuery("property graphs with incomplete edges");
      }
      throw;
    }
    const std::string predicate =
        " WHERE lower(dataset_id) = lower(" + QuoteLiteral(graph.dataset_id) +
        ") AND lower(graph_id) = lower(" + QuoteLiteral(graph.table_id) + ")";
    backend.Execute("DELETE FROM " + PropertyGraphsTable(graph.project_id) + predicate);
    if (creating) {
      backend.Execute("INSERT INTO " + PropertyGraphsTable(graph.project_id) + " VALUES (" +
                      QuoteLiteral(graph.dataset_id) + ", " + QuoteLiteral(graph.table_id) + ", " +
                      QuoteLiteral(*translation.property_graph_definition) + ", " +
                      QuoteLiteral(translation.sql) + ")");
    }
  } else if (const std::optional<std::vector<std::string>> statements =
                 AlterationStatements(translation)) {
    if (!statements->empty()) {
      backend.ExecuteDdl(statements->front(), {statements->begin() + 1, statements->end()}, "",
                         setup);
    }
  } else if (std::optional<DdlWrite> write =
                 MetadataWrite(translation, CreatedInDataset(backend, translation),
                               translation.dataset.has_value() ? location->Resolve()
                                                               : std::string(kDefaultLocation))) {
    if (translation.table.has_value()) {
      auto defaults = translation.table->schema;
      if (!translation.table->keeps_rounding_modes) {
        const auto dataset = CreatedInDataset(backend, translation);
        ApplyDefaultRoundingMode(defaults, translation.table->metadata.default_rounding_mode ==
                                                   RoundingMode::kUnspecified
                                               ? dataset.default_rounding_mode
                                               : translation.table->metadata.default_rounding_mode);
      }
      std::ranges::move(ColumnDefaultStatements(translation.table->table, defaults),
                        std::back_inserter(write->metadata_statements));
    }
    try {
      backend.ExecuteDdl(translation.sql, write->metadata_statements, write->skip_query, setup);
    } catch (const BackendError& error) {
      RoutineError(translation, error);
      throw;
    }
    // These DDL responses describe the created resource without producing a result set.
    // Read the stored schema so inherited defaults and metadata agree with tables.get.
    if (translation.table && translation.table->as_select) {
      result.schema = TableSchema(backend, translation.table->table);
    } else if (translation.view && !translation.view->materialized) {
      result.schema = TableSchema(backend, translation.view->table);
    }
  } else if (translation.ddl_target_routine.has_value()) {
    try {
      backend.Execute(translation.sql, setup);
    } catch (const BackendError& error) {
      RoutineError(translation, error);
      throw;
    }
  } else {
    std::unique_lock<std::mutex> kept;
    if (translation.written_table.has_value()) {
      kept = KeepVersion(backend, *translation.written_table);
    }
    result = backend.Execute(translation.sql, setup, null_arrays);
  }
  if (schema.has_value()) {
    result.schema = ReconcileSchema(std::move(result.schema), *schema);
  }
  if (result.has_rows) {
    result.rows.KeepResources(translation.resources);
  }
  return result;
}

}  // namespace bigquery_emulator_duckdb
