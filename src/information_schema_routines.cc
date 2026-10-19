#include "src/information_schema_routines.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "src/field_schema.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb::information_schema {

const std::vector<ViewColumn>& RoutinesColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "specific_catalog", .type = FieldType::kString},
      {.name = "specific_schema", .type = FieldType::kString},
      {.name = "specific_name", .type = FieldType::kString},
      {.name = "routine_catalog", .type = FieldType::kString},
      {.name = "routine_schema", .type = FieldType::kString},
      {.name = "routine_name", .type = FieldType::kString},
      {.name = "routine_type", .type = FieldType::kString},
      {.name = "data_type", .type = FieldType::kString},
      {.name = "routine_body", .type = FieldType::kString},
      {.name = "routine_definition", .type = FieldType::kString},
      {.name = "external_language", .type = FieldType::kString},
      {.name = "is_deterministic", .type = FieldType::kString},
      {.name = "security_type", .type = FieldType::kString},
      {.name = "created", .type = FieldType::kTimestamp},
      {.name = "last_altered", .type = FieldType::kTimestamp},
      {.name = "ddl", .type = FieldType::kString},
      {.name = "connection", .type = FieldType::kString},
  };
  return *kColumns;
}

absl::StatusOr<std::vector<Row>> RoutinesRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    for (const Routine& routine : context.source.ListRoutines(dataset)) {
      const auto& resource = routine.resource;
      const Row identity = Identity(dataset, routine.reference.routine_id);
      Row row = Concat(identity, identity);
      const auto type = resource.at("routineType").get<std::string>();
      rows.push_back(
          Concat(std::move(row),
                 {
                     String(type == "PROCEDURE"               ? "PROCEDURE"
                            : type == "TABLE_VALUED_FUNCTION" ? "TABLE FUNCTION"
                                                              : "FUNCTION"),
                     resource.contains("returnTableType")
                         ? String(StandardSqlTableTypeName(resource.at("returnTableType")))
                     : resource.contains("returnType")
                         ? String(StandardSqlTypeName(resource.at("returnType")))
                         : kNull,
                     String("SQL"),
                     String(resource.at("definitionBody").get<std::string>()),
                     kNull,
                     kNull,
                     kNull,
                     CreationTime(std::stoll(resource.at("creationTime").get<std::string>())),
                     CreationTime(std::stoll(resource.at("lastModifiedTime").get<std::string>())),
                     String(RoutineDdl(routine)),
                     kNull,
                 }));
    }
  }
  return rows;
}

const std::vector<ViewColumn>& ParametersColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "specific_catalog", .type = FieldType::kString},
      {.name = "specific_schema", .type = FieldType::kString},
      {.name = "specific_name", .type = FieldType::kString},
      {.name = "ordinal_position", .type = FieldType::kInteger},
      {.name = "parameter_mode", .type = FieldType::kString},
      {.name = "is_result", .type = FieldType::kString},
      {.name = "parameter_name", .type = FieldType::kString},
      {.name = "data_type", .type = FieldType::kString},
      {.name = "parameter_default", .type = FieldType::kString},
      {.name = "is_aggregate", .type = FieldType::kString},
  };
  return *kColumns;
}

absl::StatusOr<std::vector<Row>> ParametersRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    for (const Routine& routine : context.source.ListRoutines(dataset)) {
      const auto& resource = routine.resource;
      const bool procedure = resource.at("routineType") == "PROCEDURE";
      const Row identity = Identity(dataset, routine.reference.routine_id);
      const auto parameter = [&](int64_t ordinal, Cell mode, bool is_result, Cell name, Cell type) {
        rows.push_back(Concat(identity, {
                                            Int(ordinal),
                                            std::move(mode),
                                            String(is_result ? "YES" : "NO"),
                                            std::move(name),
                                            std::move(type),
                                            kNull,
                                            kNull,
                                        }));
      };
      if (resource.contains("returnType")) {
        parameter(0, kNull, true, kNull, String(StandardSqlTypeName(resource.at("returnType"))));
      }
      int64_t ordinal = 0;
      for (const auto& argument : resource.at("arguments")) {
        parameter(++ordinal, procedure ? String(argument.value("mode", "IN")) : kNull, false,
                  String(argument.at("name").get<std::string>()),
                  String(argument.value("argumentKind", "") == "ANY_TYPE"
                             ? "ANY TYPE"
                             : StandardSqlTypeName(argument.at("dataType"))));
      }
    }
  }
  return rows;
}

}  // namespace bigquery_emulator_duckdb::information_schema
