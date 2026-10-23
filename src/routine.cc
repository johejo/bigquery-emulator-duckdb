#include "src/routine.h"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
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

constexpr auto kContainerTypeFields = std::to_array<std::pair<std::string_view, std::string_view>>({
    {"ARRAY", "arrayElementType"},
    {"RANGE", "rangeElementType"},
    {"STRUCT", "structType"},
});

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
    if (name != "typeKind" && std::ranges::none_of(kContainerTypeFields, [&](const auto& field) {
          return kind == field.first && name == field.second;
        })) {
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
    std::vector<std::string> fields;
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
      std::string spelling;
      if (const std::string name = field.value("name", ""); !name.empty()) {
        spelling = googlesql::ToIdentifierLiteral(name) + " ";
      }
      fields.push_back(spelling + SqlTypeName(field.value("type", json::object())));
    }
    return "STRUCT<" + Join(fields, ", ") + ">";
  }
  return kind;
}

}  // namespace

std::optional<Routine> ParseRoutineComment(const RoutineReference& reference,
                                           const std::optional<std::string>& comment) {
  if (!comment.has_value()) {
    return std::nullopt;
  }
  json resource = json::parse(*comment, nullptr, /*allow_exceptions=*/false);
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

namespace {

std::string RoutineHead(const Routine& routine) {
  const json& resource = routine.resource;
  const bool procedure = resource.at("routineType") == "PROCEDURE";
  const RoutineReference& reference = routine.reference;
  std::vector<std::string> arguments;
  std::ranges::transform(resource.value("arguments", json::array()), std::back_inserter(arguments),
                         [&](const json& argument) {
                           return (procedure ? argument.value("mode", "IN") + " " : "") +
                                  googlesql::ToIdentifierLiteral(argument.value("name", "")) + " " +
                                  (argument.value("argumentKind", "") == "ANY_TYPE"
                                       ? "ANY TYPE"
                                       : SqlTypeName(argument.value("dataType", json::object())));
                         });
  std::string statement =
      std::format("CREATE {} {}.{}.{}({})",
                  procedure                                               ? "PROCEDURE"
                  : resource.at("routineType") == "TABLE_VALUED_FUNCTION" ? "TABLE FUNCTION"
                                                                          : "FUNCTION",
                  googlesql::ToIdentifierLiteral(reference.project_id),
                  googlesql::ToIdentifierLiteral(reference.dataset_id),
                  googlesql::ToIdentifierLiteral(reference.routine_id), Join(arguments, ", "));
  if (resource.contains("returnTableType")) {
    statement += " RETURNS " + StandardSqlTableTypeName(resource.at("returnTableType"));
  } else if (resource.contains("returnType")) {
    statement += " RETURNS " + SqlTypeName(resource["returnType"]);
  }
  return statement;
}

}  // namespace

std::string StandardSqlTypeName(const json& type) { return SqlTypeName(type); }

std::string StandardSqlTableTypeName(const json& type) {
  if (!type.is_object() || type.size() != 1 || !type.contains("columns") ||
      !type.at("columns").is_array() || type.at("columns").empty()) {
    throw ApiError::Invalid("Invalid routine returnTableType");
  }
  if (std::ranges::any_of(type.at("columns"), [](const json& column) {
        return !column.is_object() || column.size() != 2 || !column.contains("name") ||
               !column.at("name").is_string() || column.at("name").get<std::string>().empty() ||
               !column.contains("type");
      })) {
    throw ApiError::Invalid("Invalid routine returnTableType column");
  }
  std::vector<std::string> columns;
  std::ranges::transform(type.at("columns"), std::back_inserter(columns), [](const json& column) {
    return googlesql::ToIdentifierLiteral(column.at("name").get<std::string>()) + " " +
           SqlTypeName(column.at("type"));
  });
  return "TABLE<" + Join(columns, ", ") + ">";
}

std::string RoutineStatement(const Routine& routine) {
  const json& resource = routine.resource;
  const std::string statement = RoutineHead(routine);
  if (resource.at("routineType") == "PROCEDURE") {
    return statement +
           " OPTIONS(strict_mode = " + (resource.value("strictMode", true) ? "TRUE" : "FALSE") +
           ") " + resource.at("definitionBody").get<std::string>();
  }
  if (resource.value("language", "SQL") == "PYTHON") {
    return RoutineDdl(routine);
  }
  // On a line of its own, so that a comment at the end of the body cannot hide the parenthesis.
  return statement + " AS (" + resource.value("definitionBody", "") + "\n)";
}

std::string RoutineDdl(const Routine& routine) {
  const json& resource = routine.resource;
  std::string ddl = RoutineHead(routine);
  std::vector<std::string> options;
  const bool python = resource.value("language", "SQL") == "PYTHON";
  if (python) {
    ddl += "\nLANGUAGE python";
    options.push_back("  runtime_version=" +
                      googlesql::ToStringLiteral(resource.at("runtimeVersion").get<std::string>()));
    options.push_back("  entry_point=" +
                      googlesql::ToStringLiteral(
                          resource.at("pythonOptions").at("entryPoint").get<std::string>()));
  }
  if (resource.contains("description")) {
    options.push_back("  description=" +
                      googlesql::ToStringLiteral(resource.at("description").get<std::string>()));
  }
  if (resource.contains("strictMode")) {
    options.push_back(std::string("  strict_mode=") +
                      (resource.at("strictMode").get<bool>() ? "true" : "false"));
  }
  if (!options.empty()) {
    ddl += "\nOPTIONS(\n" + Join(options, ",\n") + ")";
  }
  const std::string body = resource.at("definitionBody").get<std::string>();
  if (python) {
    return ddl + "\nAS " + googlesql::ToStringLiteral(body) + ";";
  }
  if (resource.at("routineType") == "PROCEDURE") {
    return ddl + "\n" + body + ";";
  }
  return ddl + (resource.at("routineType") == "TABLE_VALUED_FUNCTION" ? "\nAS\n" + body + ";"
                                                                      : "\nAS (\n" + body + "\n);");
}

}  // namespace bigquery_emulator_duckdb
