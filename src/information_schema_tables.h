#pragma once

#include <array>
#include <vector>

#include "absl/status/statusor.h"
#include "src/field_schema.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"

namespace bigquery_emulator_duckdb::information_schema {

inline constexpr auto kTablesColumns = std::to_array<ViewColumn>({
    {.name = "table_catalog", .type = FieldType::kString},
    {.name = "table_schema", .type = FieldType::kString},
    {.name = "table_name", .type = FieldType::kString},
    {.name = "table_type", .type = FieldType::kString},
    {.name = "managed_table_type", .type = FieldType::kString},
    {.name = "is_insertable_into", .type = FieldType::kString},
    {.name = "is_fine_grained_mutations_enabled", .type = FieldType::kString},
    {.name = "is_typed", .type = FieldType::kString},
    {.name = "is_change_history_enabled", .type = FieldType::kString},
    {.name = "creation_time", .type = FieldType::kTimestamp},
    {.name = "base_table_catalog", .type = FieldType::kString},
    {.name = "base_table_schema", .type = FieldType::kString},
    {.name = "base_table_name", .type = FieldType::kString},
    {.name = "snapshot_time_ms", .type = FieldType::kTimestamp},
    {.name = "replica_source_catalog", .type = FieldType::kString},
    {.name = "replica_source_schema", .type = FieldType::kString},
    {.name = "replica_source_name", .type = FieldType::kString},
    {.name = "replication_status", .type = FieldType::kString},
    {.name = "replication_error", .type = FieldType::kString},
    {.name = "ddl", .type = FieldType::kString},
    {.name = "default_collation_name", .type = FieldType::kString},
    {.name = "sync_status", .type = FieldType::kJson},
    {.name = "upsert_stream_apply_watermark", .type = FieldType::kTimestamp},
});
absl::StatusOr<std::vector<Row>> TablesRows(const ViewContext& context);

inline constexpr auto kTableSnapshotsColumns = std::to_array<ViewColumn>({
    {.name = "table_catalog", .type = FieldType::kString},
    {.name = "table_schema", .type = FieldType::kString},
    {.name = "table_name", .type = FieldType::kString},
    {.name = "base_table_catalog", .type = FieldType::kString},
    {.name = "base_table_schema", .type = FieldType::kString},
    {.name = "base_table_name", .type = FieldType::kString},
    {.name = "snapshot_time", .type = FieldType::kTimestamp},
});
absl::StatusOr<std::vector<Row>> TableSnapshotsRows(const ViewContext& context);

inline constexpr auto kTableOptionsColumns = std::to_array<ViewColumn>({
    {.name = "table_catalog", .type = FieldType::kString},
    {.name = "table_schema", .type = FieldType::kString},
    {.name = "table_name", .type = FieldType::kString},
    {.name = "option_name", .type = FieldType::kString},
    {.name = "option_type", .type = FieldType::kString},
    {.name = "option_value", .type = FieldType::kString},
});
absl::StatusOr<std::vector<Row>> TableOptionsRows(const ViewContext& context);

inline constexpr auto kPartitionsColumns = std::to_array<ViewColumn>({
    {.name = "table_catalog", .type = FieldType::kString},
    {.name = "table_schema", .type = FieldType::kString},
    {.name = "table_name", .type = FieldType::kString},
    {.name = "partition_id", .type = FieldType::kString},
    {.name = "total_rows", .type = FieldType::kInteger},
    {.name = "total_logical_bytes", .type = FieldType::kInteger},
    {.name = "total_billable_bytes", .type = FieldType::kInteger},
    {.name = "last_modified_time", .type = FieldType::kTimestamp},
    {.name = "storage_tier", .type = FieldType::kString},
});
absl::StatusOr<std::vector<Row>> PartitionsRows(const ViewContext& context);

inline constexpr auto kViewsColumns = std::to_array<ViewColumn>({
    {.name = "table_catalog", .type = FieldType::kString},
    {.name = "table_schema", .type = FieldType::kString},
    {.name = "table_name", .type = FieldType::kString},
    {.name = "view_definition", .type = FieldType::kString},
    {.name = "check_option", .type = FieldType::kString},
    {.name = "use_standard_sql", .type = FieldType::kString},
});
absl::StatusOr<std::vector<Row>> ViewsRows(const ViewContext& context);

inline constexpr auto kColumnsColumns = std::to_array<ViewColumn>({
    {.name = "table_catalog", .type = FieldType::kString},
    {.name = "table_schema", .type = FieldType::kString},
    {.name = "table_name", .type = FieldType::kString},
    {.name = "column_name", .type = FieldType::kString},
    {.name = "ordinal_position", .type = FieldType::kInteger},
    {.name = "is_nullable", .type = FieldType::kString},
    {.name = "data_type", .type = FieldType::kString},
    {.name = "is_generated", .type = FieldType::kString},
    {.name = "generation_expression", .type = FieldType::kString},
    {.name = "is_stored", .type = FieldType::kString},
    {.name = "is_hidden", .type = FieldType::kString},
    {.name = "is_updatable", .type = FieldType::kString},
    {.name = "is_system_defined", .type = FieldType::kString},
    {.name = "is_partitioning_column", .type = FieldType::kString},
    {.name = "clustering_ordinal_position", .type = FieldType::kInteger},
    {.name = "collation_name", .type = FieldType::kString},
    {.name = "column_default", .type = FieldType::kString},
    {.name = "rounding_mode", .type = FieldType::kString},
    {.name = "policy_tags", .type = FieldType::kString, /*repeated=*/.repeated = true},
    {.name = "is_identity", .type = FieldType::kString},
    {.name = "identity_generation", .type = FieldType::kString},
    {.name = "identity_start", .type = FieldType::kInteger},
    {.name = "identity_increment", .type = FieldType::kInteger},
    {.name = "identity_maximum", .type = FieldType::kInteger},
    {.name = "identity_minimum", .type = FieldType::kInteger},
    {.name = "identity_cycle", .type = FieldType::kString},
});
absl::StatusOr<std::vector<Row>> ColumnsRows(const ViewContext& context);

inline constexpr auto kColumnFieldPathsColumns = std::to_array<ViewColumn>({
    {.name = "table_catalog", .type = FieldType::kString},
    {.name = "table_schema", .type = FieldType::kString},
    {.name = "table_name", .type = FieldType::kString},
    {.name = "column_name", .type = FieldType::kString},
    {.name = "field_path", .type = FieldType::kString},
    {.name = "data_type", .type = FieldType::kString},
    {.name = "description", .type = FieldType::kString},
    {.name = "collation_name", .type = FieldType::kString},
    {.name = "rounding_mode", .type = FieldType::kString},
    {.name = "policy_tags", .type = FieldType::kString, /*repeated=*/.repeated = true},
});
absl::StatusOr<std::vector<Row>> ColumnFieldPathsRows(const ViewContext& context);

}  // namespace bigquery_emulator_duckdb::information_schema
