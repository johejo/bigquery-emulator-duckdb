#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "nlohmann/json.hpp"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/references.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/ddl.h"
#include "src/translator/ddl_internal.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// The routine a persistent CREATE or DROP FUNCTION names, which BigQuery requires to be qualified
// with its dataset.
std::optional<RoutineReference> TargetRoutine(const std::vector<std::string>& path,
                                              const Scope& scope) {
  if (SplitTablePath(path).size() < 2) {
    return Unsupported(scope, "persistent function name " + Join(path, ".") + " without a dataset");
  }
  const auto parts = NormalizeTablePath(path, scope.context.defaults.project, "");
  if (parts.empty()) {
    return Unsupported(scope, "function name " + Join(path, "."));
  }
  scope.context.ddl_target_routine = RoutineReference{parts.at(0), parts.at(1), parts.at(2)};
  return scope.context.ddl_target_routine;
}

// `type` as BigQuery's StandardSqlDataType describes it, or nothing for a type BigQuery lacks.
std::optional<nlohmann::json> StandardSqlDataType(const googlesql::Type* type) {
  if (type->IsArray()) {
    auto element = StandardSqlDataType(type->AsArray()->element_type());
    if (!element) return std::nullopt;
    return nlohmann::json{{"typeKind", "ARRAY"}, {"arrayElementType", *std::move(element)}};
  }
  if (type->IsRangeType()) {
    auto element = StandardSqlDataType(type->AsRange()->element_type());
    if (!element) return std::nullopt;
    return nlohmann::json{{"typeKind", "RANGE"}, {"rangeElementType", *std::move(element)}};
  }
  if (type->IsStruct()) {
    nlohmann::json fields = nlohmann::json::array();
    for (const googlesql::StructField& field : type->AsStruct()->fields()) {
      auto field_type = StandardSqlDataType(field.type);
      if (!field_type) return std::nullopt;
      nlohmann::json entry{{"type", *std::move(field_type)}};
      if (!field.name.empty()) entry["name"] = field.name;
      fields.push_back(std::move(entry));
    }
    return nlohmann::json{{"typeKind", "STRUCT"}, {"structType", {{"fields", std::move(fields)}}}};
  }
  if (!type->IsSimpleType() || type->IsEnum()) {
    return std::nullopt;
  }
  return nlohmann::json{{"typeKind", type->TypeName(googlesql::PRODUCT_EXTERNAL)}};
}

}  // namespace

std::optional<std::string> CreateFunction(const googlesql::ResolvedCreateFunctionStmt& create,
                                          const Scope& scope) {
  if (create.create_scope() == googlesql::ResolvedCreateStatement::CREATE_TEMP) {
    return Unsupported(scope, "temporary UDFs outside multi-statement queries");
  }
  if (create.is_remote() || create.language() != "SQL") {
    return Unsupported(scope, "persistent non-SQL UDFs");
  }
  if (create.is_aggregate() || !create.aggregate_expression_list().empty() ||
      create.connection() != nullptr ||
      create.sql_security() != googlesql::ResolvedCreateStatement::SQL_SECURITY_UNSPECIFIED ||
      create.determinism_level() !=
          googlesql::ResolvedCreateFunctionStmt::DETERMINISM_UNSPECIFIED) {
    return Unsupported(scope, "UDF security, determinism or aggregates");
  }
  const std::optional<RoutineReference> target = TargetRoutine(create.name_path(), scope);
  if (!target) {
    return std::nullopt;
  }
  const std::string now = std::to_string(absl::ToUnixMillis(absl::Now()));
  nlohmann::json resource{
      {"routineType", "SCALAR_FUNCTION"},
      {"language", "SQL"},
      {"arguments", nlohmann::json::array()},
      {"definitionBody", create.code()},
      {"creationTime", now},
      {"lastModifiedTime", now},
  };
  // A function's only option is its description, which BigQuery keeps as metadata.
  for (const auto& option : create.option_list()) {
    const googlesql::Value* value = OptionLiteral(*option);
    std::string description;
    if (value == nullptr || ToLowerAscii(option->name()) != "description" ||
        !StringOption(*value, description)) {
      return Unsupported(scope, "CREATE FUNCTION option " + option->name());
    }
    if (value->is_null()) {
      resource.erase("description");
    } else {
      resource["description"] = description;
    }
  }
  const googlesql::FunctionSignature& signature = create.signature();
  for (size_t i = 0; i < create.argument_name_list().size(); ++i) {
    const googlesql::Type* type = signature.argument(static_cast<int>(i)).type();
    nlohmann::json argument{{"name", create.argument_name_list(static_cast<int>(i))}};
    if (type == nullptr) {
      argument["argumentKind"] = "ANY_TYPE";
    } else if (auto data_type = StandardSqlDataType(type)) {
      argument["dataType"] = *std::move(data_type);
    } else {
      return Unsupported(scope, "UDF arguments of type " + type->DebugString());
    }
    resource["arguments"].push_back(std::move(argument));
  }
  if (create.has_explicit_return_type()) {
    const googlesql::Type* type = signature.result_type().type();
    auto data_type = type == nullptr ? std::nullopt : StandardSqlDataType(type);
    if (!data_type) {
      return Unsupported(scope, "UDF result types other than BigQuery types");
    }
    resource["returnType"] = *std::move(data_type);
  }
  scope.context.routine = RoutineDefinition{
      .routine = {.reference = *target, .resource = std::move(resource)},
      .if_not_exists = IfNotExists(create),
  };
  const bool replace =
      create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_OR_REPLACE;
  return std::string(replace ? "CREATE OR REPLACE MACRO " : "CREATE MACRO ") +
         QualifiedName(*target) + "() AS NULL";
}

std::optional<std::string> DropFunction(const googlesql::ResolvedDropFunctionStmt& drop,
                                        const Scope& scope) {
  if (drop.arguments() != nullptr || drop.signature() != nullptr) {
    return Unsupported(scope, "DROP FUNCTION with argument types");
  }
  const std::optional<RoutineReference> target = TargetRoutine(drop.name_path(), scope);
  if (!target) {
    return std::nullopt;
  }
  return std::string(drop.is_if_exists() ? "DROP MACRO IF EXISTS " : "DROP MACRO ") +
         QualifiedName(*target);
}

}  // namespace bigquery_emulator_duckdb::translator
