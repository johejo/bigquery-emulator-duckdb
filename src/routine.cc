#include "src/routine.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/public/strings.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// The GoogleSQL spelling of the StandardSqlDataType `type`.
std::string SqlTypeName(const json& type) {
  if (!type.is_object() || !type.contains("typeKind") || !type["typeKind"].is_string()) {
    throw ApiError::Invalid("Invalid routine data type");
  }
  std::string kind = type["typeKind"].get<std::string>();
  constexpr std::array<std::string_view, 18> kinds = {
      "INT64",      "BOOL", "FLOAT64",  "STRING",   "BYTES",     "TIMESTAMP",
      "DATE",       "TIME", "DATETIME", "INTERVAL", "GEOGRAPHY", "NUMERIC",
      "BIGNUMERIC", "JSON", "ARRAY",    "STRUCT",   "RANGE",     "UUID",
      // No aliases: these are StandardSqlDataType's enum spellings.
  };
  if (std::ranges::find(kinds, kind) == kinds.end()) {
    throw ApiError::Invalid("Invalid typeKind: " + kind);
  }
  for (const auto& [name, value] : type.items()) {
    if (name != "typeKind" && !(kind == "ARRAY" && name == "arrayElementType") &&
        !(kind == "RANGE" && name == "rangeElementType") &&
        !(kind == "STRUCT" && name == "structType")) {
      throw ApiError::Invalid("Invalid routine data type field: " + name);
    }
  }
  if (kind == "ARRAY") {
    return "ARRAY<" + SqlTypeName(type.value("arrayElementType", json::object())) + ">";
  }
  if (kind == "RANGE") {
    return "RANGE<" + SqlTypeName(type.value("rangeElementType", json::object())) + ">";
  }
  if (kind == "STRUCT") {
    const json structure = type.value("structType", json::object());
    if (!structure.is_object() || structure.size() != 1 || !structure.contains("fields") ||
        !structure["fields"].is_array()) {
      throw ApiError::Invalid("Invalid routine struct type");
    }
    std::string fields;
    for (const json& field : structure["fields"]) {
      if (!field.is_object() || !field.contains("type") ||
          (field.contains("name") && !field["name"].is_string())) {
        throw ApiError::Invalid("Invalid routine struct field");
      }
      for (const auto& [name, value] : field.items()) {
        if (name != "name" && name != "type") {
          throw ApiError::Invalid("Invalid routine struct field: " + name);
        }
      }
      if (!fields.empty()) {
        fields += ", ";
      }
      if (const std::string name = field.value("name", ""); !name.empty()) {
        fields += googlesql::ToIdentifierLiteral(name) + " ";
      }
      fields += SqlTypeName(field.value("type", json::object()));
    }
    return "STRUCT<" + fields + ">";
  }
  return kind;
}

}  // namespace

std::optional<Routine> ParseRoutineComment(const RoutineReference& reference, const json& comment) {
  if (!comment.is_string()) {
    return std::nullopt;
  }
  json resource = json::parse(comment.get<std::string>(), nullptr, /*allow_exceptions=*/false);
  if (!resource.is_object() || !resource.contains("definitionBody")) {
    return std::nullopt;
  }
  return Routine{.reference = reference, .resource = std::move(resource)};
}

std::vector<std::string> RoutineCommentStatements(const Routine& routine) {
  return {
      "COMMENT ON MACRO " + QualifiedName(routine.reference) + " IS " +
          QuoteLiteral(routine.resource.dump()),
  };
}

std::string RoutinesQuery(const DatasetReference& dataset) {
  return std::format(
      "SELECT function_name, comment FROM duckdb_functions() WHERE database_name = {}"
      " AND schema_name = {} AND function_type = 'macro' ORDER BY function_name",
      QuoteLiteral(dataset.project_id), QuoteLiteral(dataset.dataset_id));
}

std::string RoutineQuery(const RoutineReference& routine) {
  return std::format(
      "SELECT comment FROM duckdb_functions() WHERE database_name = {} AND schema_name = {}"
      " AND function_name = {} AND function_type = 'macro'",
      QuoteLiteral(routine.project_id), QuoteLiteral(routine.dataset_id),
      QuoteLiteral(routine.routine_id));
}

std::string RoutineStatement(const Routine& routine) {
  const json& resource = routine.resource;
  const RoutineReference& reference = routine.reference;
  std::string arguments;
  for (const json& argument : resource.value("arguments", json::array())) {
    if (!arguments.empty()) {
      arguments += ", ";
    }
    arguments += googlesql::ToIdentifierLiteral(argument.value("name", "")) + " ";
    arguments += argument.value("argumentKind", "") == "ANY_TYPE"
                     ? "ANY TYPE"
                     : SqlTypeName(argument.value("dataType", json::object()));
  }
  std::string statement = std::format(
      "CREATE FUNCTION {}.{}.{}({})", googlesql::ToIdentifierLiteral(reference.project_id),
      googlesql::ToIdentifierLiteral(reference.dataset_id),
      googlesql::ToIdentifierLiteral(reference.routine_id), arguments);
  if (resource.contains("returnType")) {
    statement += " RETURNS " + SqlTypeName(resource["returnType"]);
  }
  // On a line of its own, so that a comment at the end of the body cannot hide the parenthesis.
  return statement + " AS (" + resource.value("definitionBody", "") + "\n)";
}

}  // namespace bigquery_emulator_duckdb
