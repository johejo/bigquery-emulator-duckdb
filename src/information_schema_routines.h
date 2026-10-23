#pragma once

#include <array>
#include <vector>

#include "absl/status/statusor.h"
#include "src/field_schema.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"

namespace bigquery_emulator_duckdb::information_schema {

inline constexpr auto kRoutinesColumns = std::to_array<ViewColumn>({
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
});
absl::StatusOr<std::vector<Row>> RoutinesRows(const ViewContext& context);

inline constexpr auto kParametersColumns = std::to_array<ViewColumn>({
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
});
absl::StatusOr<std::vector<Row>> ParametersRows(const ViewContext& context);

}  // namespace bigquery_emulator_duckdb::information_schema
