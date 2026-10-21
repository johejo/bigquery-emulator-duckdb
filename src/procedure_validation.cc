#include "src/procedure_validation.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/evaluator.h"
#include "googlesql/public/multi_catalog.h"
#include "googlesql/public/procedure.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/reference_impl/type_parameter_constraints.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/scripting/script_segment.h"
#include "googlesql/scripting/type_aliases.h"
#include "src/analyzer.h"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/language_options.h"
#include "src/routine.h"
#include "src/routine_catalog.h"
#include "src/system_variables.h"

namespace bigquery_emulator_duckdb {
namespace {

// BigQuery validates sequentially up to a control statement, DDL, CALL or dynamic SQL.
// Only scalar constants are evaluated; validation cannot read rows or execute writes.
class ProcedureValidator {
 public:
  ProcedureValidator(const std::string& body, RoutineCatalog& catalog,
                     googlesql::TypeFactory& types)
      : body_(body),
        catalog_(catalog),
        types_(types),
        system_variables_({
            {{"dataset_id"}, googlesql::Value::NullString()},
            {{"dataset_project_id"}, googlesql::Value::NullString()},
            {{"project_id"}, googlesql::Value::NullString()},
            {{"time_zone"}, googlesql::Value::NullString()},
        }) {}

  absl::Status Check(const googlesql::ASTNode& node) {
    using namespace googlesql;
    if (stopped_) {
      return absl::OkStatus();
    }
    if (node.IsDdlStatement()) {
      stopped_ = true;
      return absl::OkStatus();
    }
    switch (node.node_kind()) {
      case AST_IF_STATEMENT:
      case AST_CASE_STATEMENT:
      case AST_WHILE_STATEMENT:
      case AST_REPEAT_STATEMENT:
      case AST_FOR_IN_STATEMENT:
      case AST_RETURN_STATEMENT:
      case AST_BREAK_STATEMENT:
      case AST_CONTINUE_STATEMENT:
        stopped_ = true;
        return absl::OkStatus();
      case AST_EXCEPTION_HANDLER_LIST:
        return absl::OkStatus();
      default:
        break;
    }
    if (node.node_kind() == AST_STATEMENT_LIST) {
      const auto outer = variables_;
      const auto outer_parameters = type_parameters_;
      for (int i = 0; i < node.num_children() && !stopped_; ++i) {
        GOOGLESQL_RETURN_IF_ERROR(Check(*node.child(i)));
      }
      std::erase_if(variables_,
                    [&](const auto& variable) { return !outer.contains(variable.first); });
      type_parameters_ = outer_parameters;
      return absl::OkStatus();
    }
    if (const auto* declaration = node.GetAsOrNull<ASTVariableDeclaration>()) {
      const Type* type = nullptr;
      TypeParameters parameters;
      if (declaration->type() != nullptr) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            const auto resolved,
            AnalyzeScriptType(Segment(*declaration->type()), catalog_, types_, {}));
        type = resolved.type;
        parameters = resolved.type_params;
      }
      Value value;
      if (declaration->default_value() != nullptr) {
        GOOGLESQL_ASSIGN_OR_RETURN(value, Expression(*declaration->default_value(), type));
      } else {
        value = Value::Null(type);
      }
      GOOGLESQL_RETURN_IF_ERROR(
          googlesql::ApplyConstraints(parameters, googlesql::PRODUCT_EXTERNAL, value));
      for (const auto* name : declaration->variable_list()->identifier_list()) {
        GOOGLESQL_RETURN_IF_ERROR(AddVariable(name->GetAsString(), value));
        type_parameters_.emplace(ToLowerAscii(name->GetAsString()), parameters);
      }
      return absl::OkStatus();
    }
    if (const auto* assignment = node.GetAsOrNull<ASTSingleAssignment>()) {
      GOOGLESQL_ASSIGN_OR_RETURN(const Type* type, Variable(*assignment->variable()));
      GOOGLESQL_ASSIGN_OR_RETURN(auto value, Expression(*assignment->expression(), type));
      return Assign(*assignment->variable(), std::move(value));
    }
    if (const auto* assignment = node.GetAsOrNull<ASTAssignmentFromStruct>()) {
      std::vector<StructField> fields;
      for (const auto* name : assignment->variables()->identifier_list()) {
        GOOGLESQL_ASSIGN_OR_RETURN(const Type* type, Variable(*name));
        fields.emplace_back("", type);
      }
      const StructType* type = nullptr;
      GOOGLESQL_RETURN_IF_ERROR(types_.MakeStructType(fields, &type));
      GOOGLESQL_ASSIGN_OR_RETURN(const auto value,
                                 Expression(*assignment->struct_expression(), type));
      for (int i = 0; std::cmp_less(i, fields.size()); ++i) {
        GOOGLESQL_RETURN_IF_ERROR(
            Assign(*assignment->variables()->identifier_list(i),
                   value.is_null() ? Value::Null(fields.at(i).type) : value.field(i)));
      }
      return absl::OkStatus();
    }
    if (const auto* dynamic = node.GetAsOrNull<ASTExecuteImmediateStatement>()) {
      stopped_ = true;
      return DynamicSql(*dynamic);
    }
    if (const auto* assignment = node.GetAsOrNull<ASTSystemVariableAssignment>()) {
      const auto path = assignment->system_variable()->path()->ToIdentifierVector();
      if (path.size() != 1 || (ToLowerAscii(path.front()) != "dataset_id" &&
                               ToLowerAscii(path.front()) != "dataset_project_id")) {
        return absl::UnimplementedError(std::string(kUnsupported) +
                                        "assignment to this system variable in strict procedures");
      }
      return Expression(*assignment->expression(), types::StringType()).status();
    }
    if (const auto* statement = node.GetAsOrNull<ASTStatement>();
        statement != nullptr && statement->IsSqlStatement()) {
      stopped_ = node.Is<ASTCallStatement>();
      return WithCatalog([&](Catalog& catalog) -> absl::Status {
        GOOGLESQL_ASSIGN_OR_RETURN(
            const auto analyzed,
            AnalyzeScriptStatement(Segment(node), catalog, types_, Settings()));
        const auto* call = node.GetAsOrNull<ASTCallStatement>();
        if (call == nullptr) {
          return absl::OkStatus();
        }
        const auto& signature =
            analyzed.statement().GetAs<ResolvedCallStmt>()->procedure()->signature();
        std::set<std::string> outputs;
        for (int i = 0; std::cmp_less(i, call->arguments().size()); ++i) {
          const auto& argument = signature.argument(i);
          const auto mode = argument.options().procedure_argument_mode();
          if (mode != FunctionEnums::OUT && mode != FunctionEnums::INOUT) {
            continue;
          }
          const auto* path = call->arguments(i)->expr()->GetAsOrNull<ASTPathExpression>();
          if (path == nullptr || path->num_names() != 1) {
            return absl::InvalidArgumentError("OUT and INOUT arguments must be variables");
          }
          const auto* name = path->name(0);
          GOOGLESQL_ASSIGN_OR_RETURN(const Type* type, Variable(*name));
          if (!type->Equals(argument.type())) {
            return absl::InvalidArgumentError(
                "OUT and INOUT argument type must match variable type");
          }
          if (!outputs.insert(ToLowerAscii(name->GetAsString())).second) {
            return absl::InvalidArgumentError(
                "A variable cannot be used for multiple OUT arguments");
          }
        }
        return absl::OkStatus();
      });
    }
    if (const auto* expression = node.GetAsOrNull<ASTExpression>()) {
      return Expression(*expression, node.parent()->node_kind() == AST_RAISE_STATEMENT
                                         ? types::StringType()
                                         : nullptr)
          .status();
    }
    for (int i = 0; i < node.num_children() && !stopped_; ++i) {
      GOOGLESQL_RETURN_IF_ERROR(Check(*node.child(i)));
    }
    return absl::OkStatus();
  }

  absl::Status AddVariable(const std::string& name, const googlesql::Value& value) {
    if (!variables_.emplace(ToLowerAscii(name), value).second) {
      return absl::InvalidArgumentError("Variable redeclaration: " + name);
    }
    return absl::OkStatus();
  }

 private:
  [[nodiscard]] AnalyzerSettings Settings() const {
    return {.default_project = "", .system_variables = &system_variables_};
  }

  [[nodiscard]] googlesql::ScriptSegment Segment(const googlesql::ASTNode& node) const {
    return googlesql::ScriptSegment::FromASTNode(body_, &node);
  }

  absl::StatusOr<const googlesql::Type*> Variable(const googlesql::ASTIdentifier& name) const {
    const auto found = variables_.find(ToLowerAscii(name.GetAsString()));
    if (found == variables_.end()) {
      return absl::InvalidArgumentError("Undeclared variable: " + name.GetAsString());
    }
    return found->second.type();
  }

  template <typename F>
  auto WithCatalog(F callback) -> std::invoke_result_t<F, googlesql::Catalog&> {
    googlesql::SimpleCatalog variables("variables", &types_);
    for (const auto& [name, value] : variables_) {
      std::unique_ptr<googlesql::SimpleConstant> constant;
      GOOGLESQL_RETURN_IF_ERROR(googlesql::SimpleConstant::Create({name}, value, &constant));
      variables.AddOwnedConstant(std::move(constant));
    }
    std::unique_ptr<googlesql::MultiCatalog> catalog;
    GOOGLESQL_RETURN_IF_ERROR(
        googlesql::MultiCatalog::Create("procedure", {&variables, &catalog_}, &catalog));
    return callback(*catalog);
  }

  absl::Status Assign(const googlesql::ASTIdentifier& name, googlesql::Value value) {
    const std::string key = ToLowerAscii(name.GetAsString());
    if (const auto parameters = type_parameters_.find(key); parameters != type_parameters_.end()) {
      GOOGLESQL_RETURN_IF_ERROR(
          googlesql::ApplyConstraints(parameters->second, googlesql::PRODUCT_EXTERNAL, value));
    }
    variables_.at(key) = std::move(value);
    return absl::OkStatus();
  }

  absl::StatusOr<googlesql::Value> Expression(const googlesql::ASTExpression& expression,
                                              const googlesql::Type* target) {
    return WithCatalog([&](googlesql::Catalog& catalog) -> absl::StatusOr<googlesql::Value> {
      const auto segment = Segment(expression);
      GOOGLESQL_ASSIGN_OR_RETURN(const auto analyzed,
                                 AnalyzeScriptExpression(segment.GetSegmentText(), &segment, target,
                                                         catalog, types_, Settings()));
      std::vector<const googlesql::ResolvedNode*> nodes{&analyzed.expression()};
      while (!nodes.empty()) {
        const auto* node = nodes.back();
        nodes.pop_back();
        if (node->Is<googlesql::ResolvedParameter>() ||
            node->Is<googlesql::ResolvedSubqueryExpr>() ||
            node->Is<googlesql::ResolvedSystemVariable>() ||
            node->Is<googlesql::ResolvedColumnRef>() ||
            node->Is<googlesql::ResolvedArgumentRef>() ||
            (node->Is<googlesql::ResolvedFunctionCall>() &&
             node->GetAs<googlesql::ResolvedFunctionCall>()
                     ->function()
                     ->function_options()
                     .volatility != googlesql::FunctionEnums::IMMUTABLE)) {
          return googlesql::Value::Null(analyzed.expression().type());
        }
        std::vector<const googlesql::ResolvedNode*> children;
        node->GetChildNodes(&children);
        nodes.insert(nodes.end(), children.begin(), children.end());
      }
      googlesql::EvaluatorOptions evaluator_options;
      evaluator_options.type_factory = &types_;
      evaluator_options.default_time_zone = absl::UTCTimeZone();
      googlesql::PreparedExpression evaluator(&analyzed.expression(), evaluator_options);
      GOOGLESQL_ASSIGN_OR_RETURN(const auto options, ScriptAnalyzerOptions(Settings()));
      GOOGLESQL_RETURN_IF_ERROR(evaluator.Prepare(options, &catalog));
      return evaluator.ExecuteAfterPrepare({}, {}, system_variables_);
    });
  }

  absl::Status DynamicSql(const googlesql::ASTExecuteImmediateStatement& dynamic) {
    if (dynamic.into_clause() != nullptr) {
      for (const auto* name : dynamic.into_clause()->identifiers()->identifier_list()) {
        GOOGLESQL_RETURN_IF_ERROR(Variable(*name).status());
      }
    }
    if (dynamic.using_clause() != nullptr) {
      for (const auto* argument : dynamic.using_clause()->arguments()) {
        GOOGLESQL_RETURN_IF_ERROR(Expression(*argument->expression(), nullptr).status());
      }
    }
    GOOGLESQL_ASSIGN_OR_RETURN(const auto value,
                               Expression(*dynamic.sql(), googlesql::types::StringType()));
    if (value.is_null()) {
      return absl::OkStatus();
    }
    std::unique_ptr<googlesql::ParserOutput> parsed;
    const auto status = googlesql::ParseScript(
        value.string_value(), googlesql::ParserOptions(GoogleSqlLanguageOptions()),
        {.mode = googlesql::ERROR_MESSAGE_ONE_LINE}, &parsed);
    if (!status.ok()) {
      return absl::InvalidArgumentError("Invalid EXECUTE IMMEDIATE sql string `" +
                                        value.string_value() + "`, " +
                                        std::string(status.message()));
    }
    return absl::OkStatus();
  }

  const std::string& body_;
  RoutineCatalog& catalog_;
  googlesql::TypeFactory& types_;
  std::map<std::string, googlesql::Value> variables_;
  std::map<std::string, googlesql::TypeParameters> type_parameters_;
  googlesql::SystemVariableValuesMap system_variables_;
  bool stopped_ = false;
};

}  // namespace

absl::Status CheckProcedure(const Routine& routine, const googlesql::ASTScript& script,
                            RoutineCatalog& catalog, googlesql::TypeFactory& types) {
  const std::string body = routine.resource.at("definitionBody").get<std::string>();
  ProcedureValidator validator(body, catalog, types);
  const auto signature = AnalyzeGoogleSql(RoutineStatement(routine), catalog, types);
  const auto& create = *signature.statement().GetAs<googlesql::ResolvedCreateProcedureStmt>();
  for (int i = 0; i < create.argument_name_list_size(); ++i) {
    GOOGLESQL_RETURN_IF_ERROR(
        validator.AddVariable(create.argument_name_list(i),
                              googlesql::Value::Null(create.signature().argument(i).type())));
  }
  // These locations refer to the definition body, not the script creating the routine.
  return googlesql::MaybeUpdateErrorFromPayload(
      {
          .mode = googlesql::ERROR_MESSAGE_MULTI_LINE_WITH_CARET,
          .attach_error_location_payload = false,
      },
      body, validator.Check(script));
}

}  // namespace bigquery_emulator_duckdb
