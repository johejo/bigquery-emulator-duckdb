#include "src/analyzer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/time/time.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/sql_function.h"
#include "googlesql/public/sql_view.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/templated_sql_function.h"
#include "googlesql/public/types/annotation.h"
#include "googlesql/public/types/simple_value.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/scripting/error_helpers.h"
#include "googlesql/scripting/script_executor.h"
#include "googlesql/scripting/script_segment.h"
#include "src/ddl_option_types.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/language_options.h"
#include "src/table_path.h"
#include "src/timestamp_precision.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {

AnalyzerResult::AnalyzerResult(std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output)
    : analyzer_output_(std::move(analyzer_output)) {}

AnalyzerResult::AnalyzerResult(AnalyzerResult&&) noexcept = default;
AnalyzerResult& AnalyzerResult::operator=(AnalyzerResult&&) noexcept = default;
AnalyzerResult::~AnalyzerResult() = default;

const googlesql::ResolvedStatement& AnalyzerResult::statement() const {
  return *analyzer_output_->resolved_statement();
}

const googlesql::ResolvedExpr& AnalyzerResult::expression() const {
  return *analyzer_output_->resolved_expr();
}

const googlesql::Value* AnalyzerResult::literal_value() const {
  const auto* expr = analyzer_output_->resolved_expr();
  return expr != nullptr && expr->Is<googlesql::ResolvedLiteral>()
             ? &expr->GetAs<googlesql::ResolvedLiteral>()->value()
             : nullptr;
}

std::optional<std::vector<FieldSchema>> AnalyzerResult::result_schema() const {
  return ResultSchema(statement());
}

namespace {

constexpr auto kMaterializedViewAggregates = std::to_array<std::string_view>({
    "count",
    "$count_star",
    "countif",
    "sum",
    "min",
    "max",
    "avg",
});

// The name BigQuery reports for field `index` (0-based) of a STRUCT in a result schema.
std::string StructFieldName(const std::string& name, int index) {
  return name.empty() ? "_field_" + std::to_string(index + 1) : name;
}

// Whether `query` returns a value table of structs, whose fields BigQuery returns as columns.
bool ReturnsStructFields(const googlesql::ResolvedQueryStmt& query) {
  return query.is_value_table() && query.output_column_list_size() == 1 &&
         query.output_column_list(0)->column().type()->IsStruct();
}

// The columns of the rows `query` returns, named the way BigQuery reports them. The fields of a
// value table of structs may have empty names, which BigQuery rejects; a field whose name repeats
// an earlier one's, ignoring case, is named by its position.
std::vector<std::pair<std::string, const googlesql::Type*>> ResultColumns(
    const googlesql::ResolvedQueryStmt& query) {
  std::vector<std::pair<std::string, const googlesql::Type*>> columns;
  if (ReturnsStructFields(query)) {
    const auto& fields = query.output_column_list(0)->column().type()->AsStruct()->fields();
    std::set<std::string> names;
    for (size_t i = 0; i < fields.size(); ++i) {
      const std::string& name = fields.at(i).name;
      columns.emplace_back(name.empty() || names.insert(ToUpperAscii(name)).second
                               ? name
                               : "_field_" + std::to_string(i + 1),
                           fields.at(i).type);
    }
    return columns;
  }
  int anonymous_columns = 0;
  for (const auto& output_column : query.output_column_list()) {
    std::string name = output_column->name();
    if (googlesql::IsInternalAlias(name)) {
      name = "f" + std::to_string(anonymous_columns++) + "_";
    }
    columns.emplace_back(std::move(name), output_column->column().type());
  }
  return columns;
}

// The annotations of the columns ResultColumns returns, in its order.
std::vector<const googlesql::AnnotationMap*> ResultAnnotations(
    const googlesql::ResolvedQueryStmt& query) {
  std::vector<const googlesql::AnnotationMap*> annotations;
  if (ReturnsStructFields(query)) {
    const googlesql::AnnotationMap* row =
        query.output_column_list(0)->column().type_annotation_map();
    const googlesql::StructAnnotationMap* fields = row == nullptr ? nullptr : row->AsStructMap();
    for (int i = 0; i < query.output_column_list(0)->column().type()->AsStruct()->num_fields();
         ++i) {
      annotations.push_back(fields == nullptr ? nullptr : fields->field(i));
    }
    return annotations;
  }
  std::ranges::transform(query.output_column_list(), std::back_inserter(annotations),
                         [](const auto& output) { return output->column().type_annotation_map(); });
  return annotations;
}

}  // namespace

std::optional<std::vector<FieldSchema>> ResultSchema(
    const googlesql::ResolvedStatement& statement) {
  if (!statement.Is<googlesql::ResolvedQueryStmt>()) {
    return std::nullopt;
  }
  const auto& query = *statement.GetAs<googlesql::ResolvedQueryStmt>();
  const std::vector<const googlesql::AnnotationMap*> annotations = ResultAnnotations(query);
  std::vector<FieldSchema> schema;
  for (const auto& [name, type] : ResultColumns(query)) {
    absl::StatusOr<FieldSchema> field =
        BigQueryFieldSchema(name, type, /*allow_empty_structs=*/true);
    if (!field.ok()) {
      return std::nullopt;
    }
    SetCollations(*field, annotations.at(schema.size()));
    schema.push_back(*std::move(field));
  }
  return schema;
}

std::vector<std::string> ResultColumnNames(const googlesql::ResolvedQueryStmt& query) {
  std::vector<std::string> names;
  for (auto& [name, type] : ResultColumns(query)) {
    names.push_back(std::move(name));
  }
  return names;
}

namespace {

// Whether `node` has type parameters, such as STRING(3) or ARRAY<NUMERIC(5, 2)>.
bool HasTypeParameters(const googlesql::ASTNode& node) {
  if (node.node_kind() == googlesql::AST_TYPE_PARAMETER_LIST) {
    return true;
  }
  for (int i = 0; i < node.num_children(); ++i) {
    if (HasTypeParameters(*node.child(i))) {
      return true;
    }
  }
  return false;
}

// The first system variable that `node` refers to, if any.
const googlesql::ASTSystemVariableExpr* FindSystemVariable(const googlesql::ASTNode& node) {
  if (node.node_kind() == googlesql::AST_SYSTEM_VARIABLE_EXPR) {
    return node.GetAsOrDie<googlesql::ASTSystemVariableExpr>();
  }
  for (int i = 0; i < node.num_children(); ++i) {
    if (const auto* variable = FindSystemVariable(*node.child(i))) {
      return variable;
    }
  }
  return nullptr;
}

// BigQuery rejects type parameters in CAST, which GoogleSQL accepts. Literal CASTs disappear
// during resolution, so check the source AST.
absl::Status CheckCastTypeParameters(std::string_view sql, bool expression) {
  if (ToUpperAscii(sql).find("CAST") == std::string::npos) {
    return absl::OkStatus();
  }
  std::unique_ptr<googlesql::ParserOutput> parsed;
  const googlesql::ParserOptions options(GoogleSqlLanguageOptions());
  GOOGLESQL_RETURN_IF_ERROR(expression ? googlesql::ParseExpression(sql, options, &parsed)
                                       : googlesql::ParseStatement(sql, options, &parsed));
  std::vector<const googlesql::ASTNode*> nodes{
      expression ? static_cast<const googlesql::ASTNode*>(parsed->expression())
                 : static_cast<const googlesql::ASTNode*>(parsed->statement()),
  };
  while (!nodes.empty()) {
    const auto* node = nodes.back();
    nodes.pop_back();
    if (const auto* cast = node->GetAsOrNull<googlesql::ASTCastExpression>();
        cast != nullptr && HasTypeParameters(*cast->type())) {
      return absl::InvalidArgumentError("Parameterized types are not allowed in CAST expressions.");
    }
    for (int i = 0; i < node->num_children(); ++i) {
      nodes.push_back(node->child(i));
    }
  }
  return absl::OkStatus();
}

// COPY, CLONE and CREATE SNAPSHOT TABLE require a base-table scan, so reject views before their
// inlining changes the scan's shape. LIKE likewise needs the source's stored metadata rather than
// its query.
absl::Status CheckViewSource(const googlesql::Table* table, std::string_view operation) {
  if (dynamic_cast<const googlesql::SQLView*>(table) == nullptr) {
    return absl::OkStatus();
  }
  const auto path = SplitTablePath({table->FullName()});
  if (operation == "LIKE") {
    return absl::InvalidArgumentError(absl::StrCat("Invalid value: Cannot CREATE TABLE LIKE `",
                                                   path.at(1), ".", path.at(2),
                                                   "`, which is VIEW."));
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "Invalid value: Cannot ", operation == "SNAPSHOT" ? "snapshot " : "clone ", path.at(0), ":",
      path.at(1), ".", path.at(2), " which has type VIEW. Allowed types: [",
      operation == "COPY" ? "TABLE" : "TABLE, SNAPSHOT", "]"));
}

// Validate before rewriting: view expansion and UDF inlining would otherwise erase forms
// BigQuery does not accept in an incremental materialized view. Deliberately support only
// a single base table, optional filters/projections, and basic aggregates used directly as
// outputs. Other forms remain unsupported rather than being treated as ordinary views.
absl::Status CheckMaterializedView(const googlesql::ResolvedCreateMaterializedViewStmt& create) {
  const auto unsupported = [](std::string_view what) {
    return absl::UnimplementedError(
        absl::StrCat("The emulator does not support materialized views with ", what));
  };
  if (create.query() == nullptr) {
    return unsupported("replicas");
  }
  int tables = 0;
  std::vector<const googlesql::ResolvedNode*> nodes{create.query()};
  while (!nodes.empty()) {
    const auto* node = nodes.back();
    nodes.pop_back();
    if (node->Is<googlesql::ResolvedScan>()) {
      if (node->Is<googlesql::ResolvedTableScan>()) {
        const auto& scan = *node->GetAs<googlesql::ResolvedTableScan>();
        ++tables;
        if (dynamic_cast<const googlesql::SQLView*>(scan.table()) != nullptr ||
            scan.for_system_time_expr() != nullptr) {
          return unsupported("view references or time travel");
        }
      } else if (node->Is<googlesql::ResolvedProjectScan>()) {
        const auto& project = *node->GetAs<googlesql::ResolvedProjectScan>();
        std::vector<const googlesql::ResolvedNode*> aggregates;
        project.input_scan()->GetDescendantsWithKinds({googlesql::RESOLVED_AGGREGATE_SCAN},
                                                      &aggregates);
        if (!aggregates.empty() && std::ranges::any_of(project.expr_list(), [](const auto& column) {
              return !column->expr()->template Is<googlesql::ResolvedColumnRef>();
            })) {
          return unsupported("expressions over aggregate results");
        }
      } else if (node->Is<googlesql::ResolvedFilterScan>()) {
        std::vector<const googlesql::ResolvedNode*> aggregates;
        node->GetDescendantsWithKinds({googlesql::RESOLVED_AGGREGATE_SCAN}, &aggregates);
        if (!aggregates.empty()) {
          return unsupported("HAVING");
        }
      } else if (node->Is<googlesql::ResolvedAggregateScan>()) {
        const auto& aggregate = *node->GetAs<googlesql::ResolvedAggregateScan>();
        std::vector<const googlesql::ResolvedNode*> nested;
        aggregate.input_scan()->GetDescendantsWithKinds({googlesql::RESOLVED_AGGREGATE_SCAN},
                                                        &nested);
        if (aggregate.aggregate_list().empty() || !aggregate.grouping_set_list().empty() ||
            !nested.empty()) {
          return unsupported("DISTINCT, grouping sets or nested aggregation");
        }
      } else {
        return unsupported(node->node_kind_string());
      }
    }
    if (node->Is<googlesql::ResolvedFunctionCallBase>()) {
      const auto& call = *node->GetAs<googlesql::ResolvedFunctionCallBase>();
      if (!IsBuiltinFunction(*call.function()) ||
          call.function()->function_options().volatility != googlesql::FunctionEnums::IMMUTABLE) {
        return unsupported("UDFs or non-deterministic functions");
      }
      if (node->Is<googlesql::ResolvedAggregateFunctionCall>()) {
        const auto& aggregate = *node->GetAs<googlesql::ResolvedAggregateFunctionCall>();
        const std::string name = call.function()->Name();
        if (std::ranges::find(kMaterializedViewAggregates, name) ==
                kMaterializedViewAggregates.end() ||
            aggregate.distinct() || aggregate.having_modifier() != nullptr ||
            !aggregate.order_by_item_list().empty() || aggregate.limit() != nullptr) {
          return unsupported("this aggregate");
        }
      }
    }
    if (node->Is<googlesql::ResolvedSubqueryExpr>() || node->Is<googlesql::ResolvedParameter>() ||
        node->Is<googlesql::ResolvedSystemVariable>() ||
        node->Is<googlesql::ResolvedArgumentRef>()) {
      return unsupported("subqueries, parameters or variables");
    }
    std::vector<const googlesql::ResolvedNode*> children;
    node->GetChildNodes(&children);
    nodes.insert(nodes.end(), children.begin(), children.end());
  }
  return tables == 1 ? absl::OkStatus() : unsupported("queries without a single base table");
}

// The options a statement of a request, or of the script `settings.script`, is analyzed with.
absl::StatusOr<googlesql::AnalyzerOptions> AnalyzerOptions(TimestampPrecision precision,
                                                           const AnalyzerSettings& settings,
                                                           googlesql::ErrorMessageMode mode) {
  googlesql::AnalyzerOptions options;
  options.set_language(AnalyzerLanguageOptions(precision));
  options.enable_rewrite(googlesql::REWRITE_INLINE_SQL_FUNCTIONS);
  options.enable_rewrite(googlesql::REWRITE_INLINE_SQL_TVFS);
  // SELECT WITH DIFFERENTIAL_PRIVACY aggregates each privacy unit's contributions first, and
  // BigQuery's max_groups_contributed defaults to 1.
  options.enable_rewrite(googlesql::REWRITE_ANONYMIZATION);
  GOOGLESQL_RETURN_IF_ERROR(options.set_default_anon_kappa_value(1));
  // Keep the inliner's argument bindings as expressions. Turning them into uncorrelated
  // scalar subqueries would let DuckDB evaluate RAND() just once for an entire input table.
  if (settings.script != nullptr) {
    options.disable_rewrite(googlesql::REWRITE_WITH_EXPR);
  }
  // Stored views cannot call job-local functions. Persistent functions are resolved anew
  // when the view is read. ALTER COLUMN defaults cannot call UDFs either. Check before
  // inlining erases the function calls.
  options.AddPreRewriteCallback([](const googlesql::AnalyzerOutput& output) -> absl::Status {
    const auto* statement = output.resolved_statement();
    if (statement != nullptr && statement->Is<googlesql::ResolvedCreateMaterializedViewStmt>()) {
      GOOGLESQL_RETURN_IF_ERROR(CheckMaterializedView(
          *statement->GetAs<googlesql::ResolvedCreateMaterializedViewStmt>()));
    }
    if (statement != nullptr && statement->Is<googlesql::ResolvedCreateTableStmt>()) {
      const auto* create = statement->GetAs<googlesql::ResolvedCreateTableStmt>();
      GOOGLESQL_RETURN_IF_ERROR(CheckViewSource(create->like_table(), "LIKE"));
      for (const auto& [scan, operation] :
           {std::pair{create->clone_from(), "CLONE"}, std::pair{create->copy_from(), "COPY"}}) {
        if (scan != nullptr && scan->Is<googlesql::ResolvedTableScan>()) {
          GOOGLESQL_RETURN_IF_ERROR(
              CheckViewSource(scan->GetAs<googlesql::ResolvedTableScan>()->table(), operation));
        }
      }
    }
    if (statement != nullptr && statement->Is<googlesql::ResolvedCreateSnapshotTableStmt>()) {
      const auto* scan =
          statement->GetAs<googlesql::ResolvedCreateSnapshotTableStmt>()->clone_from();
      if (scan->Is<googlesql::ResolvedTableScan>()) {
        GOOGLESQL_RETURN_IF_ERROR(
            CheckViewSource(scan->GetAs<googlesql::ResolvedTableScan>()->table(), "SNAPSHOT"));
      }
    }
    if (statement != nullptr && statement->Is<googlesql::ResolvedCreateTableAsSelectStmt>()) {
      GOOGLESQL_RETURN_IF_ERROR(CheckViewSource(
          statement->GetAs<googlesql::ResolvedCreateTableAsSelectStmt>()->like_table(), "LIKE"));
    }
    if (statement != nullptr && (statement->Is<googlesql::ResolvedCreateViewStmt>() ||
                                 statement->Is<googlesql::ResolvedAlterTableStmt>())) {
      std::vector<const googlesql::ResolvedNode*> calls;
      statement->GetDescendantsSatisfying(
          &googlesql::ResolvedNode::Is<googlesql::ResolvedFunctionCall>, &calls);
      if (std::ranges::any_of(calls, [statement](const auto* node) {
            const auto* function =
                node->template GetAs<googlesql::ResolvedFunctionCall>()->function();
            const bool sql = function->template Is<googlesql::SQLFunction>() ||
                             function->template Is<googlesql::TemplatedSQLFunction>();
            return sql && (statement->Is<googlesql::ResolvedAlterTableStmt>() ||
                           function->FunctionNamePath().size() == 1);
          })) {
        return absl::UnimplementedError(
            statement->Is<googlesql::ResolvedCreateViewStmt>()
                ? "The emulator does not support views that call temporary SQL UDFs"
                : "The emulator does not support ALTER TABLE expressions that call SQL UDFs");
      }
    }
    return absl::OkStatus();
  });
  // BigQuery coerces an option's value to the option's type as GoogleSQL does, so that a STRING
  // literal of a timestamp sets expiration_timestamp and other STRING values are rejected, and,
  // since the option has a type, rejects a bare identifier that names a script variable.
  googlesql::AllowedHintsAndOptions allowed = options.allowed_hints_and_options();
  allowed.AddOption("expiration_timestamp", googlesql::types::TimestampType());
  // Give an untyped empty array the labels type rather than the default ARRAY<INT64>.
  allowed.AddOption("labels", DdlOptionSqlType(DdlOptionType::kLabels));
  allowed.AddOption("tags", DdlOptionSqlType(DdlOptionType::kLabels), /*allow_alter_array=*/true);
  options.set_allowed_hints_and_options(allowed);
  // BigQuery interprets civil times without an explicit zone as UTC.
  options.set_default_time_zone(absl::UTCTimeZone());
  options.set_error_message_mode(mode);
  if (!settings.positional_parameters.empty()) {
    options.set_parameter_mode(googlesql::PARAMETER_POSITIONAL);
    for (const googlesql::Type* type : settings.positional_parameters) {
      GOOGLESQL_RETURN_IF_ERROR(options.AddPositionalQueryParameter(type));
    }
  } else {
    for (const auto& [name, type] : settings.named_parameters) {
      GOOGLESQL_RETURN_IF_ERROR(options.AddQueryParameter(name, type));
    }
  }
  if (settings.script != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(settings.script->UpdateAnalyzerOptions(options));
  }
  if (settings.system_variables != nullptr) {
    for (const auto& [path, value] : *settings.system_variables) {
      GOOGLESQL_RETURN_IF_ERROR(options.AddSystemVariable(path, value.type()));
    }
  }
  return options;
}

// BigQuery creates a temporary table only in a multi-statement query, the statements of
// `script`, and names it without a qualifier other than _SESSION.
absl::Status CheckTemporaryTable(const googlesql::ResolvedStatement& statement, bool script) {
  const auto* create = dynamic_cast<const googlesql::ResolvedCreateTableStmtBase*>(&statement);
  if (create == nullptr ||
      create->create_scope() != googlesql::ResolvedCreateStatement::CREATE_TEMP) {
    return absl::OkStatus();
  }
  if (!script) {
    return absl::InvalidArgumentError("Use of CREATE TEMPORARY TABLE requires a script or session");
  }
  if (!TemporaryTableName(create->name_path()).has_value()) {
    return absl::InvalidArgumentError("Temporary tables may not be qualified");
  }
  return absl::OkStatus();
}

bool ContainsArray(const googlesql::Type* type) {
  if (type->IsArray()) {
    return true;
  }
  return type->IsStruct() &&
         std::ranges::any_of(type->AsStruct()->fields(), [](const googlesql::StructField& field) {
           return ContainsArray(field.type);
         });
}

// BigQuery groups by ARRAYs but rejects them in aggregate functions with DISTINCT, which GoogleSQL
// allows under the same language feature as grouping. It rejects STRUCTs with INTERVAL fields there
// too.
absl::Status CheckDistinctArguments(const googlesql::ResolvedNode& node) {
  std::vector<const googlesql::ResolvedNode*> calls;
  node.GetDescendantsSatisfying(
      &googlesql::ResolvedNode::Is<googlesql::ResolvedNonScalarFunctionCallBase>, &calls);
  for (const auto* descendant : calls) {
    const auto* call = descendant->GetAs<googlesql::ResolvedNonScalarFunctionCallBase>();
    if (!call->distinct()) {
      continue;
    }
    const std::string prefix = descendant->Is<googlesql::ResolvedAnalyticFunctionCall>()
                                   ? "Analytic aggregate"
                                   : "Aggregate";
    for (const auto& argument : call->argument_list()) {
      if (ContainsArray(argument->type())) {
        return absl::InvalidArgumentError(
            prefix + " functions with DISTINCT cannot be used with arguments of type ARRAY");
      }
      if (argument->type()->IsStruct() && HasInterval(argument->type())) {
        return absl::InvalidArgumentError(
            prefix + " functions with DISTINCT cannot be used with arguments of type STRUCT");
      }
    }
  }
  return absl::OkStatus();
}

// BigQuery rejects a STRUCT<> in a query's result, at any depth, naming the field by its path.
absl::Status CheckEmptyStructs(const std::string& path, const googlesql::Type* type) {
  if (type->IsArray()) {
    return CheckEmptyStructs(path, type->AsArray()->element_type());
  }
  if (!type->IsStruct()) {
    return absl::OkStatus();
  }
  const auto& fields = type->AsStruct()->fields();
  if (fields.empty()) {
    return absl::InvalidArgumentError("Unsupported empty struct type for field '" + path + "'");
  }
  for (size_t i = 0; i < fields.size(); ++i) {
    GOOGLESQL_RETURN_IF_ERROR(CheckEmptyStructs(
        path + "." + StructFieldName(fields.at(i).name, static_cast<int>(i)), fields.at(i).type));
  }
  return absl::OkStatus();
}

// Check result naming limits in both modes. BigQuery additionally rejects anonymous fields of
// STRUCT value tables and empty STRUCTs in results; GoogleSQL can return them.
absl::Status CheckResultColumns(const googlesql::ResolvedStatement& statement,
                                bool restrict_struct_results) {
  if (!statement.Is<googlesql::ResolvedQueryStmt>()) {
    return absl::OkStatus();
  }
  const auto& query = *statement.GetAs<googlesql::ResolvedQueryStmt>();
  const auto columns = ResultColumns(query);
  // BigQuery does not document the names of a field repeated more than twice, or of one whose
  // positional name another field already has.
  if (ReturnsStructFields(query)) {
    std::map<std::string, int> fields;
    std::set<std::string> names;
    for (size_t i = 0; i < columns.size(); ++i) {
      const std::string& field =
          query.output_column_list(0)->column().type()->AsStruct()->field(static_cast<int>(i)).name;
      if ((!field.empty() && ++fields[ToUpperAscii(field)] > 2) ||
          (!columns.at(i).first.empty() &&
           !names.insert(ToUpperAscii(columns.at(i).first)).second)) {
        return absl::UnimplementedError(
            "The emulator does not support these duplicate field names in a value table result: " +
            field);
      }
    }
  }
  for (const auto& [name, type] : columns) {
    if (!restrict_struct_results) {
      continue;
    }
    if (name.empty()) {
      return absl::InvalidArgumentError(
          "Invalid field name \"\". Fields must contain the allowed characters, and be at most 300 "
          "characters long. For allowed characters, please refer to "
          "https://cloud.google.com/bigquery/docs/schemas#column_names");
    }
    GOOGLESQL_RETURN_IF_ERROR(CheckEmptyStructs(name, type));
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status CheckDefinition(const googlesql::ASTNode& statement) {
  std::string function;
  if (statement.node_kind() == googlesql::AST_CREATE_FUNCTION_STATEMENT) {
    const auto* create = statement.GetAsOrDie<googlesql::ASTCreateFunctionStatement>();
    if (create->is_temp()) {
      function = absl::StrCat(
          "; in function ",
          absl::StrJoin(create->function_declaration()->name()->ToIdentifierVector(), "."));
    }
  } else if (statement.node_kind() != googlesql::AST_CREATE_VIEW_STATEMENT &&
             statement.node_kind() != googlesql::AST_CREATE_TABLE_FUNCTION_STATEMENT) {
    return absl::OkStatus();
  }
  const googlesql::ASTSystemVariableExpr* variable = FindSystemVariable(statement);
  if (variable == nullptr) {
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(
      absl::StrCat("Invalid reference to system variable @@",
                   absl::StrJoin(variable->path()->ToIdentifierVector(), "."), function));
}

AnalyzerResult AnalyzeGoogleSql(const std::string& sql, googlesql::Catalog& catalog,
                                googlesql::TypeFactory& type_factory, TimestampPrecision precision,
                                const AnalyzerSettings& settings) {
  if (!settings.named_parameters.empty() && !settings.positional_parameters.empty()) {
    throw std::invalid_argument("A statement cannot use both named and positional parameters");
  }
  absl::StatusOr<googlesql::AnalyzerOptions> options =
      AnalyzerOptions(precision, settings, googlesql::ERROR_MESSAGE_MULTI_LINE_WITH_CARET);
  if (!options.ok()) {
    throw std::invalid_argument(options.status().ToString());
  }

  // A script checks each of its statements before analyzing it.
  if (sql.find("@@") != std::string::npos) {
    std::unique_ptr<googlesql::ParserOutput> parsed;
    if (googlesql::ParseStatement(sql, googlesql::ParserOptions(GoogleSqlLanguageOptions()),
                                  &parsed)
            .ok()) {
      if (const absl::Status definition = CheckDefinition(*parsed->statement()); !definition.ok()) {
        throw std::runtime_error(std::string(definition.message()));
      }
    }
  }
  std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output;
  const absl::Status status =
      googlesql::AnalyzeStatement(sql, *options, &catalog, &type_factory, &analyzer_output);
  if (!status.ok()) {
    // These errors have been checked against BigQuery, including their location format: the
    // first line of the caret message, with " [at 1:29]" written as " at [1:29]".
    const std::string_view message = status.message();
    std::string line(message.substr(0, message.find('\n')));
    bool checked = std::ranges::any_of(
        std::array<std::string_view, 11>{
            "Unsupported primary key column ",
            "Duplicate column name ",
            "column attributes are unsupported for columns in WITH PARTITION COLUMNS clause",
            "Casting between arrays with incompatible element types",
            "DISTINCT is not allowed for function ",
            "First argument (key type) in function KEYS.NEW_KEYSET",
            "Argument 2 to BIGQUERYGOOGLESQL:TF_IDF ",
            "Argument 3 to BIGQUERYGOOGLESQL:TF_IDF ",
            "Analytic function BIGQUERYGOOGLESQL:TF_IDF ",
            "Window ORDER BY is not allowed for analytic function tf_idf",
            "Window framing clause is not allowed for analytic function tf_idf",
        },
        [&](std::string_view prefix) { return message.starts_with(prefix); });
    // Of the invalid casts, those to JSON have been checked.
    checked = checked || (line.starts_with("Invalid cast from ") &&
                          line.find(" to JSON [at ") != std::string::npos);
    // BigQuery names the element type of a RANGE that FORMAT rejects, and INTERVAL for an INTERVAL,
    // also for one in an ARRAY or, for INTERVAL, in a STRUCT of one field, and says INT64 where
    // GoogleSQL says integer; these errors have been checked as well.
    static constexpr std::string_view kGot = "; Got ";
    if (const size_t got = line.find(kGot), at = line.rfind(" [at ");
        line.starts_with("Invalid type for argument ") && got != std::string::npos &&
        at != std::string::npos && got < at && line.find(" to FORMAT; ") < got) {
      const size_t type = got + kGot.size();
      std::string_view got_type = std::string_view(line).substr(type, at - type);
      std::string reported;
      if (const size_t range = got_type.find("RANGE<"); range != std::string_view::npos) {
        if (const size_t end = got_type.find('>', range); end != std::string_view::npos) {
          reported = got_type.substr(range + 6, end - range - 6);
        }
      } else if (got_type.find(',') == std::string_view::npos) {
        while (got_type.ends_with('>')) {
          got_type.remove_suffix(1);
        }
        if (got_type.substr(got_type.find_last_of("< ") + 1) == "INTERVAL") {
          reported = "INTERVAL";
        }
      }
      if (!reported.empty()) {
        line.replace(type, at - type, reported);
        static constexpr std::string_view kInteger = "; Expected integer;";
        if (const size_t integer = line.find(kInteger); integer != std::string::npos) {
          line.replace(integer, kInteger.size(), "; Expected INT64;");
        }
        checked = true;
      }
    }
    if (checked) {
      if (const size_t at = line.rfind(" [at "); at != std::string::npos && line.ends_with(']')) {
        throw std::runtime_error(line.substr(0, at) + " at [" + line.substr(at + 5));
      }
    }
    // In the caret mode the analyzer has already folded the location into the message; this
    // only matters for the few errors that still carry it as a payload.
    const googlesql::ErrorMessageOptions error_message_options = {
        .mode = googlesql::ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET,
        .attach_error_location_payload = false,
    };
    const absl::Status formatted =
        googlesql::MaybeUpdateErrorFromPayload(error_message_options, sql, status);
    // The analyzer nests an error of the catalog's, such as EXTERNAL_QUERY's, in one of its own.
    // One that the emulator does not support is the statement's error.
    const std::string_view formatted_message = formatted.message();
    if (formatted_message.starts_with("The emulator does not support ")) {
      throw std::runtime_error(std::string(formatted_message));
    }
    if (const size_t nested = formatted_message.find("\nThe emulator does not support ");
        nested != std::string_view::npos) {
      throw std::runtime_error(std::string(formatted_message.substr(nested + 1)));
    }
    throw std::runtime_error(formatted.ToString());
  }
  if (const auto casts = CheckCastTypeParameters(sql, false); !casts.ok()) {
    throw std::runtime_error(std::string(casts.message()));
  }
  if (const absl::Status temporary =
          CheckTemporaryTable(*analyzer_output->resolved_statement(), settings.script != nullptr);
      !temporary.ok()) {
    throw std::runtime_error(std::string(temporary.message()));
  }
  if (const absl::Status distinct = CheckDistinctArguments(*analyzer_output->resolved_statement());
      !distinct.ok()) {
    throw std::runtime_error(std::string(distinct.message()));
  }
  if (const absl::Status columns = CheckResultColumns(*analyzer_output->resolved_statement(),
                                                      settings.restrict_struct_results);
      !columns.ok()) {
    throw std::runtime_error(std::string(columns.message()));
  }
  return AnalyzerResult(std::move(analyzer_output));
}

absl::StatusOr<googlesql::AnalyzerOptions> ScriptAnalyzerOptions(TimestampPrecision precision,
                                                                 const AnalyzerSettings& settings) {
  return AnalyzerOptions(precision, settings, googlesql::ERROR_MESSAGE_MULTI_LINE_WITH_CARET);
}

absl::StatusOr<googlesql::TypeWithParameters> AnalyzeScriptType(
    const googlesql::ScriptSegment& segment, googlesql::Catalog& catalog,
    googlesql::TypeFactory& type_factory, TimestampPrecision precision,
    const AnalyzerSettings& settings) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const googlesql::AnalyzerOptions options,
      AnalyzerOptions(precision, settings, googlesql::ERROR_MESSAGE_WITH_PAYLOAD));
  googlesql::TypeWithParameters type;
  googlesql::TypeModifiers modifiers;
  GOOGLESQL_RETURN_IF_ERROR(googlesql::AnalyzeType(std::string(segment.GetSegmentText()), options,
                                                   &catalog, &type_factory, &type.type, &modifiers))
      .With(googlesql::ConvertLocalErrorToScriptError(segment));
  type.type_params = modifiers.type_parameters();
  return type;
}

absl::StatusOr<AnalyzerResult> AnalyzeScriptStatement(const googlesql::ScriptSegment& segment,
                                                      googlesql::Catalog& catalog,
                                                      googlesql::TypeFactory& type_factory,
                                                      TimestampPrecision precision,
                                                      const AnalyzerSettings& settings) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const googlesql::AnalyzerOptions options,
      AnalyzerOptions(precision, settings, googlesql::ERROR_MESSAGE_WITH_PAYLOAD));
  std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output;
  GOOGLESQL_RETURN_IF_ERROR(googlesql::AnalyzeStatement(segment.GetSegmentText(), options, &catalog,
                                                        &type_factory, &analyzer_output))
      .With(googlesql::ConvertLocalErrorToScriptError(segment));
  GOOGLESQL_RETURN_IF_ERROR(CheckCastTypeParameters(segment.GetSegmentText(), false));
  GOOGLESQL_RETURN_IF_ERROR(CheckTemporaryTable(*analyzer_output->resolved_statement(), true));
  GOOGLESQL_RETURN_IF_ERROR(CheckDistinctArguments(*analyzer_output->resolved_statement()));
  GOOGLESQL_RETURN_IF_ERROR(
      CheckResultColumns(*analyzer_output->resolved_statement(), settings.restrict_struct_results));
  return AnalyzerResult(std::move(analyzer_output));
}

absl::StatusOr<AnalyzerResult> AnalyzeScriptExpression(std::string_view sql,
                                                       const googlesql::ScriptSegment* segment,
                                                       const googlesql::Type* target_type,
                                                       googlesql::Catalog& catalog,
                                                       googlesql::TypeFactory& type_factory,
                                                       TimestampPrecision precision,
                                                       const AnalyzerSettings& settings) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const googlesql::AnalyzerOptions options,
      AnalyzerOptions(precision, settings,
                      segment != nullptr ? googlesql::ERROR_MESSAGE_WITH_PAYLOAD
                                         : googlesql::ERROR_MESSAGE_ONE_LINE));
  std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output;
  const absl::Status status =
      target_type != nullptr
          ? googlesql::AnalyzeExpressionForAssignmentToType(
                sql, options, &catalog, &type_factory, target_type,
                /*target_type_modifiers=*/std::nullopt, &analyzer_output)
          : googlesql::AnalyzeExpression(sql, options, &catalog, &type_factory, &analyzer_output);
  if (segment != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(status).With(googlesql::ConvertLocalErrorToScriptError(*segment));
  }
  GOOGLESQL_RETURN_IF_ERROR(status);
  GOOGLESQL_RETURN_IF_ERROR(CheckCastTypeParameters(sql, true));
  GOOGLESQL_RETURN_IF_ERROR(CheckDistinctArguments(*analyzer_output->resolved_expr()));
  return AnalyzerResult(std::move(analyzer_output));
}

}  // namespace bigquery_emulator_duckdb
