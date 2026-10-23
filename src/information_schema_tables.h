#pragma once

#include <span>
#include <vector>

#include "absl/status/statusor.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"

namespace bigquery_emulator_duckdb::information_schema {

std::span<const ViewColumn> TablesColumns();
absl::StatusOr<std::vector<Row>> TablesRows(const ViewContext& context);

std::span<const ViewColumn> TableSnapshotsColumns();
absl::StatusOr<std::vector<Row>> TableSnapshotsRows(const ViewContext& context);

std::span<const ViewColumn> TableOptionsColumns();
absl::StatusOr<std::vector<Row>> TableOptionsRows(const ViewContext& context);

std::span<const ViewColumn> PartitionsColumns();
absl::StatusOr<std::vector<Row>> PartitionsRows(const ViewContext& context);

std::span<const ViewColumn> ViewsColumns();
absl::StatusOr<std::vector<Row>> ViewsRows(const ViewContext& context);

std::span<const ViewColumn> ColumnsColumns();
absl::StatusOr<std::vector<Row>> ColumnsRows(const ViewContext& context);

std::span<const ViewColumn> ColumnFieldPathsColumns();
absl::StatusOr<std::vector<Row>> ColumnFieldPathsRows(const ViewContext& context);

}  // namespace bigquery_emulator_duckdb::information_schema
