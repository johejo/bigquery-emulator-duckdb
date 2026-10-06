// Multi-statement queries. GoogleSQL's ScriptExecutor runs a script's control flow and keeps its
// variables; the emulator evaluates each statement and expression the script reaches, translated
// to DuckDB like the statement of a single-statement query.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "googlesql/base/status_builder.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/parser/parser.h"
#include "googlesql/proto/script_exception.pb.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/multi_catalog.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/types/struct_type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/scripting/error_helpers.h"
#include "googlesql/scripting/script_executor.h"
#include "googlesql/scripting/script_segment.h"
#include "googlesql/scripting/type_aliases.h"
#include "nlohmann/json.hpp"
#include "src/analyzer.h"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/catalog.h"
#include "src/cell_value.h"
#include "src/duckdb_sql.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/translator.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

constexpr std::string_view kUnsupported = "The emulator does not support ";

// The rows of a query that a FOR...IN loop iterates over.
class RowIterator : public googlesql::EvaluatorTableIterator {
 public:
  RowIterator(std::vector<std::string> names, std::vector<const googlesql::Type*> types,
              std::vector<std::vector<googlesql::Value>> rows)
      : names_(std::move(names)), types_(std::move(types)), rows_(std::move(rows)) {}

  int NumColumns() const override { return static_cast<int>(names_.size()); }
  std::string GetColumnName(int i) const override { return names_[i]; }
  const googlesql::Type* GetColumnType(int i) const override { return types_[i]; }
  bool NextRow() override { return ++next_ <= rows_.size(); }
  const googlesql::Value& GetValue(int i) const override { return rows_[next_ - 1][i]; }
  absl::Status Status() const override { return absl::OkStatus(); }
  absl::Status Cancel() override { return absl::OkStatus(); }

 private:
  std::vector<std::string> names_;
  std::vector<const googlesql::Type*> types_;
  std::vector<std::vector<googlesql::Value>> rows_;
  // The number of rows NextRow has moved to; the current row is the one before it.
  size_t next_ = 0;
};

// The statements of a script that the emulator does not run, with what to call them.
std::optional<std::string> UnsupportedStatement(const googlesql::ASTNode& node) {
  switch (node.node_kind()) {
    case googlesql::AST_EXECUTE_IMMEDIATE_STATEMENT:
      return "EXECUTE IMMEDIATE";
    case googlesql::AST_CALL_STATEMENT:
      return "CALL";
    case googlesql::AST_SYSTEM_VARIABLE_ASSIGNMENT:
      return "assignment to system variables";
    default:
      break;
  }
  for (int i = 0; i < node.num_children(); ++i) {
    if (auto unsupported = UnsupportedStatement(*node.child(i))) {
      return unsupported;
    }
  }
  return std::nullopt;
}

// The database that holds the temporary tables of a script while it runs: an in-memory one of
// its own, which no other job sees and which goes with the script.
class TemporaryDatabase {
 public:
  TemporaryDatabase(Backend& backend, const TemporaryTables& tables)
      : backend_(backend), name_(QuoteIdentifier(tables.project)) {
    backend_.Execute("ATTACH ':memory:' AS " + name_);
    backend_.Execute("CREATE SCHEMA " + name_ + "." + QuoteIdentifier(tables.dataset));
  }
  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;
  ~TemporaryDatabase() {
    try {
      backend_.Execute("DETACH " + name_);
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): nothing else to free.
    }
  }

 private:
  Backend& backend_;
  std::string name_;
};

}  // namespace

// Evaluates what the ScriptExecutor asks for: each statement, as a query job would run it, and
// each expression, as a query of one row and column whose value it then holds as a GoogleSQL
// value. A failure the script may handle in an EXCEPTION clause carries a ScriptException; a
// construct the emulator does not support fails the script whatever the script handles.
class ScriptEvaluator : public googlesql::StatementEvaluator {
 public:
  ScriptEvaluator(Emulator& emulator, const QueryRequest& request, const AnalyzerSettings& settings,
                  const std::vector<std::string>& setup, const TemporaryTables& temporary,
                  googlesql::TypeFactory& type_factory)
      : emulator_(emulator),
        request_(request),
        settings_(settings),
        setup_(setup),
        temporary_(temporary),
        type_factory_(type_factory) {}

  // The result of the last statement that ran, which is the script's result.
  const std::optional<QueryResult>& last_result() const { return last_result_; }

  // The error the last evaluation failed with, if it failed with one of the emulator's errors.
  const std::optional<ApiError>& failure() const { return error_; }
  void ClearFailure() { error_.reset(); }

  absl::Status ExecuteStatement(const googlesql::ScriptExecutor& executor,
                                const googlesql::ScriptSegment& segment) override {
    return Evaluate([&]() -> absl::Status {
      GOOGLESQL_ASSIGN_OR_RETURN(QueryResult result, RunStatement(executor, segment, false));
      last_result_ = std::move(result);
      return absl::OkStatus();
    });
  }

  absl::StatusOr<std::unique_ptr<googlesql::EvaluatorTableIterator>> ExecuteQueryWithResult(
      const googlesql::ScriptExecutor& executor, const googlesql::ScriptSegment& segment) override {
    std::unique_ptr<googlesql::EvaluatorTableIterator> iterator;
    GOOGLESQL_RETURN_IF_ERROR(Evaluate([&]() -> absl::Status {
      std::vector<std::string> names;
      std::vector<const googlesql::Type*> types;
      GOOGLESQL_ASSIGN_OR_RETURN(
          QueryResult result,
          RunStatement(executor, segment, true,
                       [&](const googlesql::ResolvedStatement& statement) -> absl::Status {
                         if (!statement.Is<googlesql::ResolvedQueryStmt>()) {
                           return Unsupported("FOR...IN over a statement that is not a query");
                         }
                         const auto* query = statement.GetAs<googlesql::ResolvedQueryStmt>();
                         if (query->is_value_table()) {
                           return Unsupported("FOR...IN over a value table");
                         }
                         for (const auto& column : query->output_column_list()) {
                           names.push_back(column->name());
                           types.push_back(column->column().type());
                         }
                         return absl::OkStatus();
                       }));
      std::vector<std::vector<googlesql::Value>> rows;
      for (const nlohmann::json& row : result.rows) {
        std::vector<googlesql::Value>& values = rows.emplace_back();
        for (size_t i = 0; i < types.size(); ++i) {
          GOOGLESQL_ASSIGN_OR_RETURN(values.emplace_back(),
                                     Decode(types[i], row.at("f").at(i).at("v")));
        }
      }
      iterator = std::make_unique<RowIterator>(std::move(names), std::move(types), std::move(rows));
      return absl::OkStatus();
    }));
    return iterator;
  }

  absl::StatusOr<googlesql::Value> EvaluateScalarExpression(
      const googlesql::ScriptExecutor& executor, const googlesql::ScriptSegment& segment,
      const googlesql::Type* target_type) override {
    googlesql::Value value;
    GOOGLESQL_RETURN_IF_ERROR(Evaluate([&]() -> absl::Status {
      GOOGLESQL_ASSIGN_OR_RETURN(
          value, EvaluateExpression(executor, segment.GetSegmentText(), &segment, target_type));
      return absl::OkStatus();
    }));
    return value;
  }

  absl::StatusOr<int> EvaluateCaseExpression(
      const googlesql::ScriptSegment& case_value,
      const std::vector<googlesql::ScriptSegment>& when_values,
      const googlesql::ScriptExecutor& executor) override {
    // One CASE expression evaluates the value once and compares it as SQL does.
    std::string sql = absl::StrCat("CASE (", case_value.GetSegmentText(), ")");
    for (size_t i = 0; i < when_values.size(); ++i) {
      absl::StrAppend(&sql, " WHEN (", when_values[i].GetSegmentText(), ") THEN ", i);
    }
    absl::StrAppend(&sql, " ELSE -1 END");
    googlesql::Value value;
    GOOGLESQL_RETURN_IF_ERROR(Evaluate([&]() -> absl::Status {
      GOOGLESQL_ASSIGN_OR_RETURN(value, EvaluateExpression(executor, sql, nullptr, nullptr));
      return absl::OkStatus();
    }));
    return static_cast<int>(value.int64_value());
  }

  absl::StatusOr<googlesql::TypeWithParameters> ResolveTypeName(
      const googlesql::ScriptExecutor& executor, const googlesql::ScriptSegment& segment) override {
    AnalyzerSettings settings = settings_;
    settings.script = &executor;
    std::unique_ptr<TableSource> source = emulator_.NewTableSource();
    BigQueryCatalog catalog(*source, &type_factory_, settings.default_project,
                            settings.default_dataset);
    GOOGLESQL_ASSIGN_OR_RETURN(googlesql::TypeWithParameters type,
                               AnalyzeScriptType(segment, catalog, type_factory_, settings));
    // A STRING(L) or NUMERIC(P, S) variable constrains what it holds.
    if (!type.type_params.IsEmpty()) {
      return Unsupported("variables of parameterized types");
    }
    return type;
  }

  bool IsSupportedVariableType(const googlesql::TypeWithParameters& type) override {
    return type.type_params.IsEmpty();
  }

  absl::Status ApplyTypeParameterConstraints(const googlesql::TypeParameters& type_params,
                                             googlesql::Value* /*value*/) override {
    return type_params.IsEmpty() ? absl::OkStatus()
                                 : Unsupported("variables of parameterized types");
  }

  absl::StatusOr<int64_t> GetIteratorMemoryUsage(
      const googlesql::EvaluatorTableIterator& /*iterator*/) override {
    return 0;
  }

  // The executor serializes its state only when asked to, which the emulator never does.
  absl::Status SerializeIterator(const googlesql::EvaluatorTableIterator& /*iterator*/,
                                 google::protobuf::Any& /*out*/) override {
    return absl::UnimplementedError("Script state is not serialized");
  }

  absl::StatusOr<std::unique_ptr<googlesql::EvaluatorTableIterator>> DeserializeToIterator(
      const google::protobuf::Any& /*msg*/, const googlesql::ScriptExecutor& /*executor*/,
      const googlesql::ParsedScript& /*parsed_script*/) override {
    return absl::UnimplementedError("Script state is not serialized");
  }

 private:
  static absl::Status Unsupported(std::string_view what) {
    return absl::UnimplementedError(absl::StrCat(kUnsupported, what));
  }

  // Runs `body`, turning what it throws into a status. A failure the script may handle gets a
  // ScriptException, and the emulator's error is kept for the job to fail with.
  absl::Status Evaluate(const std::function<absl::Status()>& body) {
    error_.reset();
    absl::Status status;
    try {
      status = body();
    } catch (const ApiError& error) {
      if (std::string_view(error.what()).starts_with(kUnsupported)) {
        return absl::UnimplementedError(error.what());
      }
      error_ = error;
      return googlesql::MakeScriptException() << error.what();
    } catch (const std::exception& error) {
      error_ = ApiError::InvalidQuery(error.what());
      return googlesql::MakeScriptException() << error.what();
    }
    if (status.ok() || absl::IsUnimplemented(status) || absl::IsInternal(status)) {
      return status;
    }
    error_ = ApiError::InvalidQuery(std::string(status.message()));
    return googlesql_base::StatusBuilder(status).AttachPayload(googlesql::ScriptException());
  }

  // The catalog a statement or expression of the script is analyzed against: the script's
  // variables as constants, then its temporary tables and the emulator's tables, which are the
  // defaults that the statement is translated with too.
  struct ScriptCatalog {
    std::unique_ptr<TableSource> source;
    std::unique_ptr<TemporaryTables> temporary;
    std::unique_ptr<BigQueryCatalog> tables;
    std::unique_ptr<googlesql::SimpleCatalog> variables;
    std::unique_ptr<googlesql::MultiCatalog> catalog;
  };

  absl::StatusOr<ScriptCatalog> Catalog(const googlesql::ScriptExecutor& executor) {
    ScriptCatalog catalog;
    catalog.source = emulator_.NewTableSource();
    catalog.temporary = std::make_unique<TemporaryTables>(temporary_);
    for (std::string& name : catalog.source->ListTables(temporary_.project, temporary_.dataset)) {
      catalog.temporary->names.insert(std::move(name));
    }
    catalog.tables = std::make_unique<BigQueryCatalog>(
        *catalog.source, &type_factory_, settings_.default_project, settings_.default_dataset,
        catalog.temporary.get());
    catalog.variables = std::make_unique<googlesql::SimpleCatalog>("variables", &type_factory_);
    for (const auto& [name, value] : executor.GetCurrentVariables()) {
      std::unique_ptr<googlesql::SimpleConstant> constant;
      GOOGLESQL_RETURN_IF_ERROR(
          googlesql::SimpleConstant::Create({name.ToString()}, value, &constant));
      catalog.variables->AddOwnedConstant(std::move(constant));
    }
    GOOGLESQL_RETURN_IF_ERROR(googlesql::MultiCatalog::Create(
        "script", {catalog.variables.get(), catalog.tables.get()}, &catalog.catalog));
    return catalog;
  }

  // Runs the statement `segment` as the statement of a query job, after `check` accepts its
  // resolved form. NULL arrays in the result stay null when `null_arrays` is set.
  absl::StatusOr<QueryResult> RunStatement(
      const googlesql::ScriptExecutor& executor, const googlesql::ScriptSegment& segment,
      bool null_arrays,
      const std::function<absl::Status(const googlesql::ResolvedStatement&)>& check = nullptr) {
    AnalyzerSettings settings = settings_;
    settings.script = &executor;
    GOOGLESQL_ASSIGN_OR_RETURN(ScriptCatalog catalog, Catalog(executor));
    GOOGLESQL_ASSIGN_OR_RETURN(
        AnalyzerResult analyzed,
        AnalyzeScriptStatement(segment, *catalog.catalog, type_factory_, settings));
    if (check) {
      GOOGLESQL_RETURN_IF_ERROR(check(analyzed.statement()));
    }
    std::string unsupported;
    const std::optional<TranslatedStatement> translation = TranslateStatement(
        analyzed.statement(), request_.parameters,
        DefaultDataset{settings.default_project, settings.default_dataset, catalog.temporary.get()},
        &unsupported, &executor.GetKnownSystemVariables());
    if (!translation.has_value()) {
      return Unsupported(unsupported);
    }
    return emulator_.RunStatement(*translation, setup_, null_arrays);
  }

  // Evaluates the expression `sql`, the text of `segment` when it is given, coerced to
  // `target_type` when that is given.
  absl::StatusOr<googlesql::Value> EvaluateExpression(const googlesql::ScriptExecutor& executor,
                                                      std::string_view sql,
                                                      const googlesql::ScriptSegment* segment,
                                                      const googlesql::Type* target_type) {
    AnalyzerSettings settings = settings_;
    settings.script = &executor;
    GOOGLESQL_ASSIGN_OR_RETURN(ScriptCatalog catalog, Catalog(executor));
    // SET (a, b) = ... assigns the fields of a STRUCT without field names, which DuckDB cannot
    // hold; the fields are named for DuckDB, and the value then takes the unnamed type. They take
    // the names the expression gives them, if it does, since DuckDB casts a STRUCT by field name
    // where GoogleSQL casts it by position.
    if (target_type != nullptr && target_type->IsStruct()) {
      std::vector<googlesql::StructField> fields = target_type->AsStruct()->fields();
      if (std::ranges::any_of(fields, [](const auto& field) { return field.name.empty(); })) {
        const absl::StatusOr<AnalyzerResult> own = AnalyzeScriptExpression(
            sql, segment, nullptr, *catalog.catalog, type_factory_, settings);
        const googlesql::StructType* own_type = own.ok() && own->expression().type()->IsStruct()
                                                    ? own->expression().type()->AsStruct()
                                                    : nullptr;
        std::set<std::string> names;
        if (own_type != nullptr && own_type->num_fields() == static_cast<int>(fields.size())) {
          std::ranges::transform(own_type->fields(), std::inserter(names, names.end()),
                                 [](const auto& field) { return ToLowerAscii(field.name); });
        }
        const bool own_names = names.size() == fields.size() && !names.contains("");
        for (size_t i = 0; i < fields.size(); ++i) {
          fields[i].name = own_names ? own_type->field(static_cast<int>(i)).name
                                     : absl::StrCat("_field_", i + 1);
        }
        const googlesql::StructType* named = nullptr;
        GOOGLESQL_RETURN_IF_ERROR(type_factory_.MakeStructType(fields, &named));
        GOOGLESQL_ASSIGN_OR_RETURN(googlesql::Value value,
                                   EvaluateExpression(executor, sql, segment, named));
        if (value.is_null()) {
          return googlesql::Value::Null(target_type);
        }
        std::vector<googlesql::Value> values;
        values.reserve(value.num_fields());
        for (int i = 0; i < value.num_fields(); ++i) {
          values.push_back(value.field(i));
        }
        return googlesql::Value::MakeStruct(target_type->AsStruct(), std::move(values));
      }
    }
    GOOGLESQL_ASSIGN_OR_RETURN(AnalyzerResult analyzed,
                               AnalyzeScriptExpression(sql, segment, target_type, *catalog.catalog,
                                                       type_factory_, settings));
    std::string unsupported;
    const std::optional<std::string> query = TranslateExpression(
        analyzed.expression(), request_.parameters,
        DefaultDataset{settings.default_project, settings.default_dataset, catalog.temporary.get()},
        &unsupported, &executor.GetKnownSystemVariables());
    if (!query.has_value()) {
      return Unsupported(unsupported);
    }
    const QueryResult result = emulator_.Execute(*query, setup_, true);
    return Decode(analyzed.expression().type(), result.rows.at(0).at("f").at(0).at("v"));
  }

  // A value of a result, which the emulator cannot hold in a variable when it cannot decode it.
  static absl::StatusOr<googlesql::Value> Decode(const googlesql::Type* type,
                                                 const nlohmann::json& cell) {
    absl::StatusOr<googlesql::Value> value = CellValue(type, cell);
    if (!value.ok()) {
      return Unsupported("values of type " + type->DebugString() + " in variables");
    }
    return value;
  }

  Emulator& emulator_;
  const QueryRequest& request_;
  const AnalyzerSettings& settings_;
  const std::vector<std::string>& setup_;
  const TemporaryTables& temporary_;
  googlesql::TypeFactory& type_factory_;
  std::optional<QueryResult> last_result_;
  std::optional<ApiError> error_;
};

std::unique_ptr<googlesql::ParserOutput> Emulator::ParseScript(const std::string& query) {
  std::unique_ptr<googlesql::ParserOutput> output;
  if (!googlesql::ParseScript(query, googlesql::ParserOptions(GoogleSqlLanguageOptions()),
                              {.mode = googlesql::ERROR_MESSAGE_ONE_LINE}, &output)
           .ok()) {
    return nullptr;
  }
  // BigQuery runs one statement on its own; anything else is a multi-statement query.
  const auto& statements = output->script()->statement_list();
  if (statements.size() == 1 && statements[0]->IsSqlStatement()) {
    return nullptr;
  }
  return statements.empty() ? nullptr : std::move(output);
}

QueryResult Emulator::RunScript(const QueryRequest& request, const googlesql::ParserOutput& script,
                                const std::string& default_project,
                                const std::string& default_dataset,
                                const std::vector<std::string>& setup) {
  if (request.dry_run) {
    throw ApiError::InvalidQuery(std::string(kUnsupported) + "dry runs of multi-statement queries");
  }
  if (request.destination_table.has_value()) {
    throw ApiError::Invalid("Cannot set destination table in multi-statement queries");
  }
  if (!request.parameters.positional_types().empty()) {
    throw ApiError::InvalidQuery(std::string(kUnsupported) +
                                 "positional parameters in multi-statement queries");
  }
  if (const auto unsupported = UnsupportedStatement(*script.script())) {
    throw ApiError::InvalidQuery(std::string(kUnsupported) + *unsupported);
  }

  googlesql::TypeFactory type_factory;
  AnalyzerSettings settings{.default_project = default_project, .default_dataset = default_dataset};
  for (const FieldSchema& field : request.parameters.named_types()) {
    absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
    if (!type.ok()) {
      throw ApiError::InvalidQuery(std::string(type.status().message()));
    }
    settings.named_parameters.emplace_back(field.name, *type);
  }
  absl::StatusOr<googlesql::AnalyzerOptions> analyzer_options = ScriptAnalyzerOptions(settings);
  if (!analyzer_options.ok()) {
    throw ApiError::InvalidQuery(std::string(analyzer_options.status().message()));
  }
  googlesql::ScriptExecutorOptions options;
  options.PopulateFromAnalyzerOptions(*analyzer_options);
  options.set_type_factory(&type_factory);

  const TemporaryTables temporary{.project = "_script_" + std::to_string(next_script_number_++),
                                  .dataset = kSessionDataset};
  const TemporaryDatabase database(backend_, temporary);
  ScriptEvaluator evaluator(*this, request, settings, setup, temporary, type_factory);
  absl::StatusOr<std::unique_ptr<googlesql::ScriptExecutor>> executor =
      googlesql::ScriptExecutor::CreateFromAST(request.query, script.script(), options, &evaluator);
  if (!executor.ok()) {
    throw ApiError::InvalidQuery(std::string(executor.status().message()));
  }
  while (!(*executor)->IsComplete()) {
    evaluator.ClearFailure();
    const absl::Status status = (*executor)->ExecuteNext();
    if (!status.ok()) {
      const std::string message(status.message());
      if (evaluator.failure().has_value() && !absl::IsUnimplemented(status)) {
        throw evaluator.failure()->WithMessage(message);
      }
      throw ApiError::InvalidQuery(message);
    }
  }
  return evaluator.last_result().value_or(QueryResult{});
}

}  // namespace bigquery_emulator_duckdb
