// Multi-statement queries. GoogleSQL's ScriptExecutor runs a script's control flow and keeps its
// variables; the emulator evaluates each statement and expression the script reaches, translated
// to DuckDB like the statement of a single-statement query.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
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
#include "googlesql/scripting/stack_frame.h"
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
#include "src/javascript_function.h"
#include "src/query_parameters.h"
#include "src/translated_statement.h"
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
  std::string GetColumnName(int i) const override { return names_.at(i); }
  const googlesql::Type* GetColumnType(int i) const override { return types_.at(i); }
  bool NextRow() override { return ++next_ <= rows_.size(); }
  const googlesql::Value& GetValue(int i) const override { return rows_.at(next_ - 1).at(i); }
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

// The statements of a script that the emulator does not run, with what to call them.
std::optional<std::string> UnsupportedStatement(const googlesql::ASTNode& node) {
  switch (node.node_kind()) {
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
        type_factory_(type_factory),
        functions_("temporary functions", &type_factory),
        backend_(emulator.backend_.NewSession()) {}

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
    AnalyzerSettings settings = settings_;
    settings.script = &executor;
    std::unique_ptr<TableSource> source = emulator_.NewTableSource(backend_.get());
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

  // BigQuery runs one SQL statement as dynamic SQL, but no control statement, CALL or another
  // EXECUTE IMMEDIATE; the emulator runs neither the other scripting statements nor transactions
  // there, which OnProcedureEntered then rejects before the statement runs.
  bool IsAllowedAsDynamicSql(const googlesql::ASTStatement* statement) override {
    unsupported_dynamic_sql_.reset();
    switch (statement->node_kind()) {
      case googlesql::AST_EXECUTE_IMMEDIATE_STATEMENT:
      case googlesql::AST_CALL_STATEMENT:
      case googlesql::AST_IF_STATEMENT:
      case googlesql::AST_CASE_STATEMENT:
      case googlesql::AST_WHILE_STATEMENT:
      case googlesql::AST_REPEAT_STATEMENT:
      case googlesql::AST_FOR_IN_STATEMENT:
        return false;
      case googlesql::AST_BEGIN_STATEMENT:
      case googlesql::AST_COMMIT_STATEMENT:
      case googlesql::AST_ROLLBACK_STATEMENT:
        unsupported_dynamic_sql_ = "transactions in EXECUTE IMMEDIATE";
        break;
      default:
        if (!statement->IsSqlStatement()) {
          unsupported_dynamic_sql_ = "scripting statements in EXECUTE IMMEDIATE";
        }
        break;
    }
    return true;
  }

  // Called as EXECUTE IMMEDIATE enters its statement, which is the only procedure the emulator
  // runs. An unsupported construct fails the script, which the script cannot handle.
  absl::Status OnProcedureEntered(const googlesql::ScriptExecutor& /*executor*/,
                                  const absl::Span<const std::string>& /*path*/) override {
    const std::optional<std::string> unsupported =
        std::exchange(unsupported_dynamic_sql_, std::nullopt);
    if (unsupported.has_value()) {
      return Unsupported(*unsupported);
    }
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

  absl::StatusOr<ScriptCatalog> Catalog(const googlesql::ScriptExecutor& executor,
                                        bool list_temporary = true) {
    ScriptCatalog catalog;
    catalog.source = emulator_.NewTableSource(backend_.get());
    catalog.temporary = std::make_unique<TemporaryTables>(temporary_);
    if (list_temporary) {
      for (std::string& name : catalog.source->ListTables(temporary_.project, temporary_.dataset)) {
        catalog.temporary->names.insert(std::move(name));
      }
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
    if (create.create_scope() != googlesql::ResolvedCreateStatement::CREATE_TEMP) {
      return Unsupported("persistent UDFs");
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
      definitions_.push_back({std::move(catalog), std::move(analyzed), nullptr, nullptr});
      return absl::OkStatus();
    }
    // An ANY TYPE argument defers resolving the body to each call. Resolve it as a typed body
    // is resolved, without the script's variables or functions defined later, itself included.
    FunctionDefinition definition{std::move(catalog), std::move(analyzed), nullptr, nullptr};
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
    settings = settings_;
    settings.script = &executor;
    const googlesql::StackFrame* frame = executor.GetCurrentStackFrame();
    if (frame == nullptr || !frame->is_dynamic_sql()) {
      return request_.parameters;
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
    AnalyzerSettings settings;
    GOOGLESQL_ASSIGN_OR_RETURN(const QueryParameters parameters, Parameters(executor, settings));
    // Transaction control needs no tables, and ROLLBACK must work on an aborted connection.
    const bool control = segment.node()->Is<googlesql::ASTBeginStatement>() ||
                         segment.node()->Is<googlesql::ASTCommitStatement>() ||
                         segment.node()->Is<googlesql::ASTRollbackStatement>();
    GOOGLESQL_ASSIGN_OR_RETURN(ScriptCatalog catalog, Catalog(executor, !control));
    // A function body resolves against tables and previously defined functions, never the
    // script's variables (which would otherwise be captured as constants).
    if (segment.node()->node_kind() == googlesql::AST_CREATE_FUNCTION_STATEMENT) {
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
    // BigQuery permits only queries, DML and DDL on temporary tables in a transaction.
    if (in_transaction_) {
      if (statement.Is<googlesql::ResolvedCreateFunctionStmt>()) {
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
    if (analyzed.statement().Is<googlesql::ResolvedCreateFunctionStmt>()) {
      GOOGLESQL_RETURN_IF_ERROR(CreateFunction(std::move(catalog), std::move(analyzed)));
      return QueryResult{};
    }
    std::string unsupported;
    const std::optional<TranslatedStatement> translation =
        TranslateStatement(analyzed.statement(), parameters,
                           DefaultDataset{
                               settings.default_project,
                               settings.default_dataset,
                               catalog.temporary.get(),
                               emulator_.has_session_user_,
                           },
                           &unsupported, &executor.GetKnownSystemVariables());
    if (!translation.has_value()) {
      return Unsupported(unsupported);
    }
    return emulator_.RunStatement(*translation, setup_, null_arrays, backend_.get());
  }

  // Evaluates the expression `sql`, the text of `segment` when it is given, coerced to
  // `target_type` when that is given.
  absl::StatusOr<googlesql::Value> EvaluateExpression(const googlesql::ScriptExecutor& executor,
                                                      std::string_view sql,
                                                      const googlesql::ScriptSegment* segment,
                                                      const googlesql::Type* target_type) {
    AnalyzerSettings settings;
    GOOGLESQL_ASSIGN_OR_RETURN(const QueryParameters parameters, Parameters(executor, settings));
    GOOGLESQL_ASSIGN_OR_RETURN(ScriptCatalog catalog, Catalog(executor));
    GOOGLESQL_ASSIGN_OR_RETURN(AnalyzerResult analyzed,
                               AnalyzeScriptExpression(sql, segment, target_type, *catalog.catalog,
                                                       type_factory_, settings));
    std::string unsupported;
    const std::optional<std::string> query =
        TranslateExpression(analyzed.expression(), parameters,
                            DefaultDataset{
                                settings.default_project,
                                settings.default_dataset,
                                catalog.temporary.get(),
                                emulator_.has_session_user_,
                            },
                            &unsupported, &executor.GetKnownSystemVariables());
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

  Emulator& emulator_;
  const QueryRequest& request_;
  const AnalyzerSettings& settings_;
  const std::vector<std::string>& setup_;
  const TemporaryTables& temporary_;
  googlesql::TypeFactory& type_factory_;
  std::vector<FunctionDefinition> definitions_;
  googlesql::SimpleCatalog functions_;
  std::unique_ptr<Backend> backend_;
  bool in_transaction_ = false;
  bool transaction_failed_ = false;
  std::optional<QueryResult> last_result_;
  std::optional<ApiError> error_;
  // What the emulator does not support in the statement that EXECUTE IMMEDIATE is about to run.
  std::optional<std::string> unsupported_dynamic_sql_;
};

std::unique_ptr<googlesql::ParserOutput> Emulator::ParseScript(const std::string& query) {
  std::unique_ptr<googlesql::ParserOutput> output;
  if (!googlesql::ParseScript(query, googlesql::ParserOptions(GoogleSqlLanguageOptions()),
                              {.mode = googlesql::ERROR_MESSAGE_ONE_LINE}, &output)
           .ok()) {
    return nullptr;
  }
  // A TEMP function needs a job-local catalog even when it is the only statement, and EXECUTE
  // IMMEDIATE, which GoogleSQL parses as a SQL statement, runs as a script.
  const auto& statements = output->script()->statement_list();
  if (statements.size() == 1 && statements.at(0)->IsSqlStatement() &&
      statements.at(0)->node_kind() != googlesql::AST_EXECUTE_IMMEDIATE_STATEMENT) {
    const auto* create = statements.at(0)->GetAsOrNull<googlesql::ASTCreateFunctionStatement>();
    if (create == nullptr || !create->is_temp()) {
      return nullptr;
    }
  }
  return statements.empty() ? nullptr : std::move(output);
}

std::string Emulator::ScriptStatementType(const googlesql::ParserOutput& script) {
  const auto& statements = script.script()->statement_list();
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

  const TemporaryTables temporary{
      .project = "_script_" + std::to_string(next_script_number_++),
      .dataset = kSessionDataset,
  };
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
