#include "src/information_schema_routines.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb::information_schema {

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
                     String(resource.value("language", "SQL") == "PYTHON" ? "EXTERNAL" : "SQL"),
                     String(resource.at("definitionBody").get<std::string>()),
                     resource.value("language", "SQL") == "PYTHON" ? String("PYTHON") : kNull,
                     resource.value("language", "SQL") == "PYTHON" ? String("NO") : kNull,
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
