// Multi-statement queries. GoogleSQL's ScriptExecutor runs a script's control flow and keeps its
// variables; the emulator evaluates each statement and expression the script reaches, translated
// to DuckDB like the statement of a single-statement query.

#include "src/script.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "google/protobuf/any.pb.h"
#include "googlesql/base/status_builder.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/parser/parser.h"
#include "googlesql/proto/script_exception.pb.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/function.h"
#include "googlesql/public/multi_catalog.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/sql_function.h"
#include "googlesql/public/templated_sql_function.h"
#include "googlesql/public/types/struct_type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/types/type_parameters.h"
#include "googlesql/public/value.h"
#include "googlesql/reference_impl/type_parameter_constraints.h"
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
#include "src/field_schema.h"
#include "src/javascript_function.h"
#include "src/query_parameters.h"
#include "src/routine.h"
#include "src/routine_catalog.h"
#include "src/system_variables.h"
#include "src/translated_statement.h"
#include "src/translator.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

// The rows of a query that a FOR...IN loop iterates over.
class RowIterator : public googlesql::EvaluatorTableIterator {
 public:
  RowIterator(std::vector<std::string> names, std::vector<const googlesql::Type*> types,
              std::vector<std::vector<googlesql::Value>> rows)
      : names_(std::move(names)), types_(std::move(types)), rows_(std::move(rows)) {}

  [[nodiscard]] int NumColumns() const override { return static_cast<int>(names_.size()); }
  [[nodiscard]] std::string GetColumnName(int i) const override { return names_.at(i); }
  [[nodiscard]] const googlesql::Type* GetColumnType(int i) const override { return types_.at(i); }
  bool NextRow() override { return ++next_ <= rows_.size(); }
  [[nodiscard]] const googlesql::Value& GetValue(int i) const override {
    return rows_.at(next_ - 1).at(i);
  }
  absl::Status Status() const override { return absl::OkStatus(); }
  absl::Status Cancel() override { return absl::OkStatus(); }

 private:
  std::vector<std::string> names_;
  std::vector<const googlesql::Type*> types_;
  std::vector<std::vector<googlesql::Value>> rows_;
  // The number of rows NextRow has moved to; the current row is the one before it.
  size_t next_ = 0;
};

// The types a JavaScript UDF takes and returns in the emulator so far, and its argument names,
// which must be JavaScript identifiers to name the parameters of the function the body becomes.
// BigQuery rejects INT64 arguments, since JavaScript numbers cannot hold every INT64, but returns
// INT64 results.
absl::Status CheckJavaScriptSignature(const googlesql::FunctionSignature& signature,
                                      const std::vector<std::string>& argument_names) {
  for (const auto& name : argument_names) {
    const auto identifier = [](char c, bool first) {
      return absl::ascii_isalpha(c) || c == '_' || c == '$' || (!first && absl::ascii_isdigit(c));
    };
    if (name.empty() || !identifier(name.front(), true) ||
        !std::ranges::all_of(name, [&](char c) { return identifier(c, false); })) {
      return absl::UnimplementedError(
          absl::StrCat(kUnsupported, "JavaScript UDF argument name ", name));
    }
  }
  for (const auto& argument : signature.arguments()) {
    const googlesql::Type* type = argument.type();
    if (type != nullptr && type->IsInt64()) {
      return absl::InvalidArgumentError(
          "INT64 is not supported as an argument type of JavaScript UDFs; use FLOAT64 or STRING");
    }
    if (type == nullptr || !(type->IsBool() || type->IsDouble() || type->IsString())) {
      return absl::UnimplementedError(
          absl::StrCat(kUnsupported, "JavaScript UDF arguments of type ",
                       type == nullptr ? "ANY TYPE" : type->DebugString()));
    }
  }
  const googlesql::Type* result = signature.result_type().type();
  if (result == nullptr ||
      !(result->IsBool() || result->IsDouble() || result->IsString() || result->IsInt64())) {
    return absl::UnimplementedError(
        absl::StrCat(kUnsupported, "JavaScript UDF results of type ",
                     result == nullptr ? "ANY TYPE" : result->DebugString()));
  }
  return absl::OkStatus();
}

// Whether `path` names the read-only @@project_id, the project that runs the script.
bool IsProjectId(const std::vector<std::string>& path) {
  return path.size() == 1 && absl::EqualsIgnoreCase(path.front(), "project_id");
}

// The statements of a script that the emulator does not run, with what to call them.
std::optional<std::string> UnsupportedStatement(const googlesql::ASTNode& node) {
  if (node.node_kind() == googlesql::AST_SYSTEM_VARIABLE_ASSIGNMENT) {
    const auto path = node.GetAsOrDie<googlesql::ASTSystemVariableAssignment>()
                          ->system_variable()
                          ->path()
                          ->ToIdentifierVector();
    if (path.size() != 1 ||
        (!absl::EqualsIgnoreCase(path.front(), "dataset_id") &&
         !absl::EqualsIgnoreCase(path.front(), "dataset_project_id") && !IsProjectId(path))) {
      return "assignment to system variables other than @@dataset_id and @@dataset_project_id";
    }
  }
  for (int i = 0; i < node.num_children(); ++i) {
    if (auto unsupported = UnsupportedStatement(*node.child(i))) {
      return unsupported;
    }
  }
  return std::nullopt;
}

// Whether BigQuery runs `statement` as dynamic SQL, which it does not for a control statement,
// CALL, EXECUTE IMMEDIATE or BEGIN...END block. Sets `unsupported` to what the emulator does not
// run of the statements it does.
bool AllowedAsDynamicSql(const googlesql::ASTStatement& statement,
                         std::optional<std::string>& unsupported) {
  switch (statement.node_kind()) {
    case googlesql::AST_EXECUTE_IMMEDIATE_STATEMENT:
    case googlesql::AST_CALL_STATEMENT:
    case googlesql::AST_IF_STATEMENT:
    case googlesql::AST_CASE_STATEMENT:
    case googlesql::AST_WHILE_STATEMENT:
    case googlesql::AST_REPEAT_STATEMENT:
    case googlesql::AST_FOR_IN_STATEMENT:
    case googlesql::AST_BEGIN_END_BLOCK:
      return false;
    case googlesql::AST_BEGIN_STATEMENT:
    case googlesql::AST_COMMIT_STATEMENT:
    case googlesql::AST_ROLLBACK_STATEMENT:
      unsupported = "transactions in EXECUTE IMMEDIATE";
      return true;
    default:
      unsupported = UnsupportedStatement(statement);
      if (!unsupported && !statement.IsSqlStatement()) {
        unsupported = "scripting statements in EXECUTE IMMEDIATE";
      }
      return true;
  }
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

// A frame of the script's call stack: the script itself, a procedure that CALL entered, or the
// statement that EXECUTE IMMEDIATE runs. The evaluator enters and leaves frames with the
// executor, whose frames hold the variables. Temporary tables, temporary functions and system
// variables belong to the whole script.
struct ScriptFrame {
  enum class Kind : uint8_t { kScript, kProcedure, kDynamicSql };
  Kind kind = Kind::kScript;
  // The project of the procedure that the frame runs in, which its names resolve against; empty
  // outside procedures.
  std::string procedure_project;
};

// Evaluates what the ScriptExecutor asks for: each statement, as a query job would run it, and
// each expression, as a query of one row and column whose value it then holds as a GoogleSQL
// value. A failure the script may handle in an EXCEPTION clause carries a ScriptException; a
// construct the emulator does not support fails the script whatever the script handles.
class ScriptEvaluator : public googlesql::StatementEvaluator {
 public:
  ScriptEvaluator(ScriptHost& host, const QueryParameters& parameters,
                  const AnalyzerSettings& settings, const std::vector<std::string>& setup,
                  const TemporaryTables& temporary, googlesql::TypeFactory& type_factory,
                  const std::string& query_project, googlesql::SystemVariableValuesMap& variables)
      : host_(host),
        parameters_(parameters),
        settings_(settings),
        setup_(setup),
        temporary_(temporary),
        type_factory_(type_factory),
        query_project_(query_project),
        system_variables_(variables),
        functions_("temporary functions", &type_factory),
        backend_(host.backend().NewSession()) {}

  // The result of the last statement that ran, which is the script's result.
  const std::optional<QueryResult>& last_result() const { return last_result_; }

  // The error the last evaluation failed with, if it failed with one of the emulator's errors.
  const std::optional<ApiError>& failure() const { return error_; }
  void ClearFailure() { error_.reset(); }

  absl::Status AssignSystemVariable(googlesql::ScriptExecutor* /*executor*/,
                                    const googlesql::ASTSystemVariableAssignment* assignment,
                                    const googlesql::Value& value) override {
    const auto path = assignment->system_variable()->path()->ToIdentifierVector();
    if (IsProjectId(path)) {
      return googlesql::AssignmentToReadOnlySystemVariable(assignment);
    }
    const auto variable = system_variables_.find(path);
    if (variable == system_variables_.end()) {
      return Unsupported(
          "assignment to system variables other than @@dataset_id and @@dataset_project_id");
    }
    variable->second = value;
    assigned_system_variables_.insert(path);
    return absl::OkStatus();
  }

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
          const QueryResult result,
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
                                     Decode(types.at(i), row.at("f").at(i).at("v")));
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
      absl::StrAppend(&sql, " WHEN (", when_values.at(i).GetSegmentText(), ") THEN ", i);
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
    const AnalyzerSettings settings = Settings(executor);
    std::unique_ptr<TableSource> source = host_.NewTableSource(backend_.get(), /*expire=*/false);
    BigQueryCatalog catalog(*source, &type_factory_, settings.default_project,
                            settings.default_dataset);
    return AnalyzeScriptType(segment, catalog, type_factory_, settings);
  }

  bool IsSupportedVariableType(const googlesql::TypeWithParameters& /*type*/) override {
    return true;
  }

  // A STRING(L) or BYTES(L) variable fails to take a longer value, and a NUMERIC(P, S) or
  // BIGNUMERIC(P, S) one rounds a value to S digits and fails to take one of more than P, as
  // GoogleSQL's reference implementation does.
  absl::Status ApplyTypeParameterConstraints(const googlesql::TypeParameters& type_params,
                                             googlesql::Value* value) override {
    return googlesql::ApplyConstraints(type_params, googlesql::PRODUCT_EXTERNAL, *value);
  }

  // BigQuery runs one SQL statement as dynamic SQL; what the emulator does not run of those,
  // OnProcedureEntered rejects before the statement runs.
  bool IsAllowedAsDynamicSql(const googlesql::ASTStatement* statement) override {
    std::optional<std::string> unsupported;
    if (!AllowedAsDynamicSql(*statement, unsupported)) {
      return false;
    }
    const std::string& procedure_project = frames_.back().procedure_project;
    pending_frame_ = PendingFrame{
        .frame = {.kind = ScriptFrame::Kind::kDynamicSql, .procedure_project = procedure_project},
        .status = unsupported                 ? Unsupported(*unsupported)
                  : procedure_project.empty() ? absl::OkStatus()
                                              : CheckProcedureBody(*statement, /*dynamic=*/true),
    };
    return true;
  }

  absl::StatusOr<std::unique_ptr<googlesql::ProcedureDefinition>> LoadProcedure(
      const googlesql::ScriptExecutor& executor, const absl::Span<const std::string>& path,
      const int64_t /*num_arguments*/) override {
    pending_frame_.reset();
    const auto settings = Settings(executor);
    const auto source = host_.NewTableSource(backend_.get(), false);
    const auto parts = NormalizeTablePath(SplitTablePath(path), settings.default_project,
                                          settings.default_dataset, source.get());
    if (parts.size() != 3) {
      return absl::NotFoundError("Procedure name must include a dataset");
    }
    const auto routine = source->FindRoutine(
        {.project_id = parts.at(0), .dataset_id = parts.at(1), .routine_id = parts.at(2)});
    if (!routine || routine->resource.at("routineType") != "PROCEDURE") {
      return absl::NotFoundError("Procedure is not found");
    }
    return LoadProcedureDefinition(*routine, *source);
  }

  // Enters the frame that LoadProcedure or IsAllowedAsDynamicSql prepared, just before the
  // executor pushes its own. What they found unsupported fails here, where the error propagates
  // without becoming catchable.
  absl::Status OnProcedureEntered(const googlesql::ScriptExecutor& /*executor*/,
                                  const absl::Span<const std::string>& /*path*/) override {
    std::optional<PendingFrame> pending = std::exchange(pending_frame_, std::nullopt);
    GOOGLESQL_RET_CHECK(pending.has_value());
    GOOGLESQL_RETURN_IF_ERROR(pending->status);
    frames_.push_back(std::move(pending->frame));
    return absl::OkStatus();
  }

  // The executor leaves a frame on return and when an error unwinds it into a handler.
  absl::Status OnProcedureExited(const googlesql::ScriptExecutor& /*executor*/,
                                 const absl::Span<const std::string>& /*path*/) override {
    GOOGLESQL_RET_CHECK_GT(frames_.size(), 1);
    frames_.pop_back();
    return absl::OkStatus();
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

  absl::StatusOr<std::unique_ptr<googlesql::ProcedureDefinition>> LoadProcedureDefinition(
      const Routine& routine, TableSource& source) {
    try {
      RoutineCatalog catalog(source, &type_factory_, routine.reference.project_id, "");
      const auto analyzed = AnalyzeGoogleSql(RoutineStatement(routine), catalog, type_factory_,
                                             {.default_project = routine.reference.project_id});
      const auto& create = *analyzed.statement().GetAs<googlesql::ResolvedCreateProcedureStmt>();
      std::unique_ptr<googlesql::ParserOutput> parsed;
      GOOGLESQL_RETURN_IF_ERROR(googlesql::ParseScript(
          create.procedure_body(), googlesql::ParserOptions(GoogleSqlLanguageOptions()),
          {.mode = googlesql::ERROR_MESSAGE_ONE_LINE}, &parsed));
      pending_frame_ = PendingFrame{
          .frame =
              {
                  .kind = ScriptFrame::Kind::kProcedure,
                  .procedure_project = routine.reference.project_id,
              },
          .status = CheckProcedureBody(*parsed->script()),
      };
      return std::make_unique<googlesql::ProcedureDefinition>(
          routine.reference.project_id + "." + routine.reference.dataset_id + "." +
              routine.reference.routine_id,
          create.signature(), create.argument_name_list(), create.procedure_body());
    } catch (const std::exception& error) {
      return absl::InvalidArgumentError(error.what());
    }
  }

  // Checks the body of a procedure, or with `dynamic` a statement that EXECUTE IMMEDIATE runs in
  // one, whose query parameters are those of its USING clause.
  static absl::Status CheckProcedureBody(const googlesql::ASTNode& node, bool dynamic = false) {
    // Only reads of @@dataset_id have been checked against BigQuery in a procedure, and
    // @@project_id is documented as the project that runs the script.
    if (const auto* variable = node.GetAsOrNull<googlesql::ASTSystemVariableExpr>()) {
      const auto path = variable->path()->ToIdentifierVector();
      if (path.size() != 1 ||
          (!absl::EqualsIgnoreCase(path.front(), "dataset_id") && !IsProjectId(path))) {
        return Unsupported(
            "system variables other than @@dataset_id and @@project_id inside procedures");
      }
    }
    if (node.node_kind() == googlesql::AST_CREATE_FUNCTION_STATEMENT ||
        node.node_kind() == googlesql::AST_SYSTEM_VARIABLE_ASSIGNMENT ||
        (!dynamic && node.node_kind() == googlesql::AST_PARAMETER_EXPR)) {
      return Unsupported(
          "function declarations, system variable assignments and query parameters inside "
          "procedures");
    }
    for (int i = 0; i < node.num_children(); ++i) {
      GOOGLESQL_RETURN_IF_ERROR(CheckProcedureBody(*node.child(i), dynamic));
    }
    return absl::OkStatus();
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
      transaction_failed_ = in_transaction_;
      error_ = error;
      return googlesql::MakeScriptException() << error.what();
    } catch (const std::exception& error) {
      if (in_transaction_ &&
          std::string_view(error.what())
                  .find("a single transaction can only write to a single attached database") !=
              std::string_view::npos) {
        return Unsupported("transactions that write to multiple databases");
      }
      transaction_failed_ = in_transaction_;
      error_ = ApiError::InvalidQuery(error.what());
      return googlesql::MakeScriptException() << error.what();
    }
    if (status.ok() || absl::IsUnimplemented(status) || absl::IsInternal(status)) {
      return status;
    }
    transaction_failed_ = in_transaction_;
    error_ = ApiError::InvalidQuery(std::string(status.message()));
    return googlesql_base::StatusBuilder(status).AttachPayload(googlesql::ScriptException());
  }

  AnalyzerSettings Settings(const googlesql::ScriptExecutor& executor) const {
    AnalyzerSettings settings = settings_;
    settings.script = &executor;
    const auto& project = system_variables_.at({"dataset_project_id"});
    const auto& dataset = system_variables_.at({"dataset_id"});
    settings.default_project = project.is_null() ? query_project_ : project.string_value();
    settings.default_dataset = dataset.is_null() ? "" : dataset.string_value();
    if (const ScriptFrame& frame = frames_.back(); !frame.procedure_project.empty()) {
      settings.default_project = frame.procedure_project;
      settings.default_dataset.clear();
      settings.named_parameters.clear();
    }
    return settings;
  }

  googlesql::SystemVariableValuesMap SystemVariables(
      const googlesql::ScriptExecutor& executor) const {
    auto variables = executor.GetKnownSystemVariables();
    // Initial reads of the dataset variables with a request default dataset remain unsupported
    // until checked against BigQuery. Without a request default, both are confirmed to start as
    // NULL. @@project_id is read-only and known from the start.
    for (const auto& [path, value] : system_variables_) {
      if (settings_.default_dataset.empty() || assigned_system_variables_.contains(path) ||
          IsProjectId(path)) {
        variables.emplace(path, value);
      }
    }
    return variables;
  }

  // The catalog a statement or expression of the script is analyzed against: the script's
  // variables as constants, then its temporary tables and the emulator's tables, which are the
  // defaults that the statement is translated with too.
  struct ScriptCatalog {
    std::unique_ptr<TableSource> source;
    std::unique_ptr<TemporaryTables> temporary;
    std::unique_ptr<RoutineCatalog> tables;
    std::unique_ptr<googlesql::SimpleCatalog> variables;
    std::unique_ptr<googlesql::MultiCatalog> catalog;
  };

  absl::StatusOr<ScriptCatalog> Catalog(const googlesql::ScriptExecutor& executor,
                                        const AnalyzerSettings& settings,
                                        bool list_temporary = true) {
    ScriptCatalog catalog;
    catalog.source = host_.NewTableSource(backend_.get(), /*expire=*/list_temporary);
    catalog.temporary = std::make_unique<TemporaryTables>(temporary_);
    if (list_temporary) {
      for (std::string& name : catalog.source->ListTables(temporary_.project, temporary_.dataset)) {
        catalog.temporary->names.insert(std::move(name));
      }
    }
    catalog.tables =
        std::make_unique<RoutineCatalog>(*catalog.source, &type_factory_, settings.default_project,
                                         settings.default_dataset, catalog.temporary.get());
    catalog.variables = std::make_unique<googlesql::SimpleCatalog>("variables", &type_factory_);
    for (const auto& [name, value] : executor.GetCurrentVariables()) {
      std::unique_ptr<googlesql::SimpleConstant> constant;
      GOOGLESQL_RETURN_IF_ERROR(
          googlesql::SimpleConstant::Create({name.ToString()}, value, &constant));
      catalog.variables->AddOwnedConstant(std::move(constant));
    }
    GOOGLESQL_RETURN_IF_ERROR(googlesql::MultiCatalog::Create(
        "script", {catalog.variables.get(), &functions_, catalog.tables.get()}, &catalog.catalog));
    return catalog;
  }

  // SQLFunction borrows its body; keep both the analysis and its catalog alive until the
  // script ends. Bodies may also reference tables and previously defined functions.
  // A templated function resolves its body at each call, against `functions`, those defined
  // before it, and the tables of `catalog`.
  struct FunctionDefinition {
    ScriptCatalog catalog;
    AnalyzerResult analyzed;
    std::unique_ptr<googlesql::SimpleCatalog> functions;
    std::unique_ptr<googlesql::MultiCatalog> body_catalog;
  };

  absl::Status CreateFunction(ScriptCatalog catalog, AnalyzerResult analyzed) {
    const auto& create = *analyzed.statement().GetAs<googlesql::ResolvedCreateFunctionStmt>();
    if (!create.hint_list().empty()) {
      return Unsupported("statement hints");
    }
    const bool javascript = absl::EqualsIgnoreCase(create.language(), "js");
    if (create.is_remote() || (!javascript && create.language() != "SQL")) {
      return Unsupported("non-SQL UDFs");
    }
    if (create.is_aggregate() || !create.aggregate_expression_list().empty() ||
        !create.option_list().empty() || create.connection() != nullptr ||
        create.sql_security() != googlesql::ResolvedCreateStatement::SQL_SECURITY_UNSPECIFIED ||
        create.determinism_level() !=
            googlesql::ResolvedCreateFunctionStmt::DETERMINISM_UNSPECIFIED) {
      return Unsupported("UDF options, security, determinism or aggregates");
    }
    if (create.name_path().size() != 1 ||
        create.name_path().front().find('.') != std::string::npos) {
      return absl::InvalidArgumentError("Temporary function names must not be qualified");
    }
    if (create.create_mode() != googlesql::ResolvedCreateStatement::CREATE_DEFAULT) {
      return Unsupported("OR REPLACE and IF NOT EXISTS for temporary SQL UDFs");
    }
    const std::string& name = create.name_path().front();
    const googlesql::Function* existing = nullptr;
    GOOGLESQL_RETURN_IF_ERROR(functions_.GetFunction(name, &existing));
    if (existing != nullptr) {
      return absl::InvalidArgumentError("Already Exists: Function " + name);
    }
    if (javascript) {
      GOOGLESQL_RETURN_IF_ERROR(
          CheckJavaScriptSignature(create.signature(), create.argument_name_list()));
      functions_.AddOwnedFunction(std::make_unique<JavaScriptFunction>(
          create.name_path(), create.signature(), create.argument_name_list(), create.code()));
      return absl::OkStatus();
    }
    if (create.function_expression() != nullptr) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          auto function, googlesql::SQLFunction::Create(
                             create.name_path(), googlesql::Function::SCALAR, create.signature(),
                             googlesql::FunctionOptions(), create.function_expression(),
                             create.argument_name_list()));
      functions_.AddOwnedFunction(std::move(function));
      definitions_.push_back({
          .catalog = std::move(catalog),
          .analyzed = std::move(analyzed),
          .functions = nullptr,
          .body_catalog = nullptr,
      });
      return absl::OkStatus();
    }
    // An ANY TYPE argument defers resolving the body to each call. Resolve it as a typed body
    // is resolved, without the script's variables or functions defined later, itself included.
    FunctionDefinition definition{
        .catalog = std::move(catalog),
        .analyzed = std::move(analyzed),
        .functions = nullptr,
        .body_catalog = nullptr,
    };
    definition.functions = std::make_unique<googlesql::SimpleCatalog>("functions", &type_factory_);
    for (const googlesql::Function* function : functions_.functions()) {
      definition.functions->AddFunction(function);
    }
    GOOGLESQL_RETURN_IF_ERROR(googlesql::MultiCatalog::Create(
        "function body", {definition.functions.get(), definition.catalog.tables.get()},
        &definition.body_catalog));
    auto function = std::make_unique<googlesql::TemplatedSQLFunction>(
        create.name_path(), create.signature(), create.argument_name_list(),
        googlesql::ParseResumeLocation::FromStringView(create.code()));
    function->set_resolution_catalog(definition.body_catalog.get());
    functions_.AddOwnedFunction(std::move(function));
    definitions_.push_back(std::move(definition));
    return absl::OkStatus();
  }

  // Sets `settings` to those that what the script runs now is analyzed with, and returns its query
  // parameters: those of the request, but in dynamic SQL only the ones that EXECUTE IMMEDIATE
  // passes with USING, which the executor adds to the analyzer options itself.
  absl::StatusOr<QueryParameters> Parameters(const googlesql::ScriptExecutor& executor,
                                             AnalyzerSettings& settings) {
    settings = Settings(executor);
    switch (frames_.back().kind) {
      case ScriptFrame::Kind::kScript:
        return parameters_;
      case ScriptFrame::Kind::kProcedure:
        return QueryParameters();
      case ScriptFrame::Kind::kDynamicSql:
        break;
    }
    settings.named_parameters.clear();
    const auto& values = executor.GetCurrentParameterValues();
    if (!values.has_value()) {
      return QueryParameters();
    }
    std::string unsupported;
    std::optional<QueryParameters> parameters = TranslateParameters(*values, &unsupported);
    if (!parameters.has_value()) {
      return Unsupported(unsupported);
    }
    return *std::move(parameters);
  }

  // Runs the statement `segment` as the statement of a query job, after `check` accepts its
  // resolved form. NULL arrays in the result stay null when `null_arrays` is set.
  absl::StatusOr<QueryResult> RunStatement(
      const googlesql::ScriptExecutor& executor, const googlesql::ScriptSegment& segment,
      bool null_arrays,
      const std::function<absl::Status(const googlesql::ResolvedStatement&)>& check = nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(CheckDefinition(*segment.node()));
    AnalyzerSettings settings;
    GOOGLESQL_ASSIGN_OR_RETURN(const QueryParameters parameters, Parameters(executor, settings));
    // Transaction control needs no tables, and ROLLBACK must work on an aborted connection.
    const bool control = segment.node()->Is<googlesql::ASTBeginStatement>() ||
                         segment.node()->Is<googlesql::ASTCommitStatement>() ||
                         segment.node()->Is<googlesql::ASTRollbackStatement>();
    GOOGLESQL_ASSIGN_OR_RETURN(ScriptCatalog catalog, Catalog(executor, settings, !control));
    // A function body resolves against tables and previously defined functions, never the
    // script's variables (which would otherwise be captured as constants).
    if (segment.node()->node_kind() == googlesql::AST_CREATE_FUNCTION_STATEMENT ||
        segment.node()->node_kind() == googlesql::AST_CREATE_TABLE_FUNCTION_STATEMENT) {
      GOOGLESQL_RETURN_IF_ERROR(googlesql::MultiCatalog::Create(
          "function body", {&functions_, catalog.tables.get()}, &catalog.catalog));
    }
    GOOGLESQL_ASSIGN_OR_RETURN(
        AnalyzerResult analyzed,
        AnalyzeScriptStatement(segment, *catalog.catalog, type_factory_, settings));
    const auto& statement = analyzed.statement();
    if (statement.Is<googlesql::ResolvedBeginStmt>()) {
      const auto* begin = statement.GetAs<googlesql::ResolvedBeginStmt>();
      if (begin->read_write_mode() != googlesql::ResolvedBeginStmt::MODE_UNSPECIFIED ||
          !begin->isolation_level_list().empty()) {
        return Unsupported("transaction modes");
      }
      backend_->Transaction("BEGIN TRANSACTION");
      in_transaction_ = true;
      transaction_failed_ = false;
      return QueryResult{};
    }
    if (statement.Is<googlesql::ResolvedCommitStmt>() ||
        statement.Is<googlesql::ResolvedRollbackStmt>()) {
      const bool commit = statement.Is<googlesql::ResolvedCommitStmt>();
      if (commit && transaction_failed_) {
        return Unsupported("committing a transaction after a handled error; roll it back instead");
      }
      backend_->Transaction(commit ? "COMMIT" : "ROLLBACK");
      in_transaction_ = false;
      transaction_failed_ = false;
      return QueryResult{};
    }
    const bool temporary_function =
        statement.Is<googlesql::ResolvedCreateFunctionStmt>() &&
        statement.GetAs<googlesql::ResolvedCreateFunctionStmt>()->create_scope() ==
            googlesql::ResolvedCreateStatement::CREATE_TEMP;
    // BigQuery permits only queries, DML and DDL on temporary tables in a transaction.
    if (in_transaction_) {
      if (temporary_function) {
        return Unsupported("SQL UDF declarations in transactions");
      }
      const bool temporary_create =
          (statement.Is<googlesql::ResolvedCreateTableStmt>() &&
           statement.GetAs<googlesql::ResolvedCreateTableStmt>()->create_scope() ==
               googlesql::ResolvedCreateStatement::CREATE_TEMP) ||
          (statement.Is<googlesql::ResolvedCreateTableAsSelectStmt>() &&
           statement.GetAs<googlesql::ResolvedCreateTableAsSelectStmt>()->create_scope() ==
               googlesql::ResolvedCreateStatement::CREATE_TEMP);
      bool temporary_drop = false;
      if (statement.Is<googlesql::ResolvedDropStmt>()) {
        const auto* drop = statement.GetAs<googlesql::ResolvedDropStmt>();
        const auto& path = drop->name_path();
        temporary_drop =
            drop->object_type() == "TABLE" && !path.empty() &&
            (path.size() == 1 || (path.size() == 2 && path.at(0) == kSessionDataset)) &&
            catalog.temporary->names.contains(path.back());
      }
      if (!statement.Is<googlesql::ResolvedQueryStmt>() &&
          !statement.Is<googlesql::ResolvedInsertStmt>() &&
          !statement.Is<googlesql::ResolvedUpdateStmt>() &&
          !statement.Is<googlesql::ResolvedDeleteStmt>() &&
          !statement.Is<googlesql::ResolvedMergeStmt>() &&
          !statement.Is<googlesql::ResolvedTruncateStmt>() && !temporary_create &&
          !temporary_drop) {
        throw ApiError::InvalidQuery(
            "DDL statements on permanent objects are not allowed in a transaction");
      }
    }
    if (check) {
      GOOGLESQL_RETURN_IF_ERROR(check(analyzed.statement()));
    }
    if (temporary_function) {
      GOOGLESQL_RETURN_IF_ERROR(CreateFunction(std::move(catalog), std::move(analyzed)));
      return QueryResult{};
    }
    std::string unsupported;
    const auto variables = SystemVariables(executor);
    const std::optional<TranslatedStatement> translation =
        TranslateStatement(analyzed.statement(), parameters,
                           DefaultDataset{
                               .project = settings.default_project,
                               .dataset = settings.default_dataset,
                               .temporary = catalog.temporary.get(),
                               .source = catalog.source.get(),
                               .has_session_user = host_.has_session_user(),
                               .evaluate =
                                   [this](const std::string& sql, const googlesql::Type* type) {
                                     return ResultValue(backend_->Execute(sql, setup_, true), type);
                                   },
                           },
                           &unsupported, &variables);
    if (!translation.has_value()) {
      return Unsupported(unsupported);
    }
    if (translation->routine.has_value()) {
      GOOGLESQL_RETURN_IF_ERROR(catalog.tables->CheckRoutine(translation->routine->routine));
    }
    if (translation->view) {
      GOOGLESQL_RETURN_IF_ERROR(
          catalog.tables->CheckView(translation->view->table, translation->view->query));
    }
    return host_.RunStatement(*translation, setup_, null_arrays, backend_.get());
  }

  // Evaluates the expression `sql`, the text of `segment` when it is given, coerced to
  // `target_type` when that is given.
  absl::StatusOr<googlesql::Value> EvaluateExpression(const googlesql::ScriptExecutor& executor,
                                                      std::string_view sql,
                                                      const googlesql::ScriptSegment* segment,
                                                      const googlesql::Type* target_type) {
    AnalyzerSettings settings;
    GOOGLESQL_ASSIGN_OR_RETURN(const QueryParameters parameters, Parameters(executor, settings));
    GOOGLESQL_ASSIGN_OR_RETURN(ScriptCatalog catalog, Catalog(executor, settings));
    GOOGLESQL_ASSIGN_OR_RETURN(const AnalyzerResult analyzed,
                               AnalyzeScriptExpression(sql, segment, target_type, *catalog.catalog,
                                                       type_factory_, settings));
    std::string unsupported;
    const auto variables = SystemVariables(executor);
    const std::optional<std::string> query =
        TranslateExpression(analyzed.expression(), parameters,
                            DefaultDataset{
                                .project = settings.default_project,
                                .dataset = settings.default_dataset,
                                .temporary = catalog.temporary.get(),
                                .has_session_user = host_.has_session_user(),
                            },
                            &unsupported, &variables);
    if (!query.has_value()) {
      return Unsupported(unsupported);
    }
    const QueryResult result = backend_->Execute(*query, setup_, true);
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

  ScriptHost& host_;
  const QueryParameters& parameters_;
  const AnalyzerSettings& settings_;
  const std::vector<std::string>& setup_;
  const TemporaryTables& temporary_;
  googlesql::TypeFactory& type_factory_;
  const std::string& query_project_;
  googlesql::SystemVariableValuesMap& system_variables_;
  std::set<std::vector<std::string>, googlesql::StringVectorCaseLess> assigned_system_variables_;
  std::vector<FunctionDefinition> definitions_;
  googlesql::SimpleCatalog functions_;
  std::unique_ptr<Backend> backend_;
  bool in_transaction_ = false;
  bool transaction_failed_ = false;
  std::optional<QueryResult> last_result_;
  std::optional<ApiError> error_;
  // The frames the script runs in, the innermost last.
  std::vector<ScriptFrame> frames_{1};
  // The frame that the procedure or dynamic SQL statement the executor is about to enter runs in,
  // with what the emulator does not support in it.
  struct PendingFrame {
    ScriptFrame frame;
    absl::Status status;
  };
  std::optional<PendingFrame> pending_frame_;
};

}  // namespace

QueryResult RunScript(ScriptHost& host, const std::string& query,
                      const googlesql::ParserOutput& script, const QueryParameters& parameters,
                      const std::string& default_project, const std::string& default_dataset,
                      const std::vector<std::string>& setup, const std::string& query_project,
                      int64_t number) {
  if (!parameters.positional_types().empty()) {
    throw ApiError::UnsupportedQuery("positional parameters in multi-statement queries");
  }
  if (const auto unsupported = UnsupportedStatement(*script.script())) {
    throw ApiError::UnsupportedQuery(*unsupported);
  }

  googlesql::TypeFactory type_factory;
  googlesql::SystemVariableValuesMap variables{
      {
          {"dataset_project_id"},
          default_dataset.empty() ? googlesql::Value::NullString()
                                  : googlesql::Value::String(default_project),
      },
      {
          {"dataset_id"},
          default_dataset.empty() ? googlesql::Value::NullString()
                                  : googlesql::Value::String(default_dataset),
      },
      {{"project_id"}, googlesql::Value::String(query_project)},
  };
  AnalyzerSettings settings{
      .default_project = default_project,
      .default_dataset = default_dataset,
      .system_variables = &variables,
  };
  for (const FieldSchema& field : parameters.named_types()) {
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

  const TemporaryTables temporary{
      .project = "_script_" + std::to_string(number),
      .dataset = kSessionDataset,
  };
  const TemporaryDatabase database(host.backend(), temporary);
  ScriptEvaluator evaluator(host, parameters, settings, setup, temporary, type_factory,
                            query_project, variables);
  absl::StatusOr<std::unique_ptr<googlesql::ScriptExecutor>> executor =
      googlesql::ScriptExecutor::CreateFromAST(query, script.script(), options, &evaluator);
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
