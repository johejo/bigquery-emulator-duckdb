#pragma once

#include <array>
#include <vector>

#include "absl/status/statusor.h"
#include "src/field_schema.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"

namespace bigquery_emulator_duckdb::information_schema {

inline constexpr auto kSchemataColumns = std::to_array<ViewColumn>({
    {.name = "catalog_name", .type = FieldType::kString},
    {.name = "schema_name", .type = FieldType::kString},
    {.name = "schema_owner", .type = FieldType::kString},
    {.name = "creation_time", .type = FieldType::kTimestamp},
    {.name = "last_modified_time", .type = FieldType::kTimestamp},
    {.name = "location", .type = FieldType::kString},
    {.name = "ddl", .type = FieldType::kString},
    {.name = "default_collation_name", .type = FieldType::kString},
    {.name = "sync_status", .type = FieldType::kJson},
});
absl::StatusOr<std::vector<Row>> SchemataRows(const ViewContext& context);

inline constexpr auto kSchemataOptionsColumns = std::to_array<ViewColumn>({
    {.name = "catalog_name", .type = FieldType::kString},
    {.name = "schema_name", .type = FieldType::kString},
    {.name = "option_name", .type = FieldType::kString},
    {.name = "option_type", .type = FieldType::kString},
    {.name = "option_value", .type = FieldType::kString},
});
absl::StatusOr<std::vector<Row>> SchemataOptionsRows(const ViewContext& context);

}  // namespace bigquery_emulator_duckdb::information_schema
