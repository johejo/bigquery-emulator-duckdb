#pragma once

#include <vector>

#include "absl/status/statusor.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"

namespace bigquery_emulator_duckdb::information_schema {

const std::vector<ViewColumn>& TablesColumns();
absl::StatusOr<std::vector<Row>> TablesRows(const ViewContext& context);

const std::vector<ViewColumn>& TableSnapshotsColumns();
absl::StatusOr<std::vector<Row>> TableSnapshotsRows(const ViewContext& context);

const std::vector<ViewColumn>& TableOptionsColumns();
absl::StatusOr<std::vector<Row>> TableOptionsRows(const ViewContext& context);

const std::vector<ViewColumn>& PartitionsColumns();
absl::StatusOr<std::vector<Row>> PartitionsRows(const ViewContext& context);

const std::vector<ViewColumn>& ViewsColumns();
absl::StatusOr<std::vector<Row>> ViewsRows(const ViewContext& context);

const std::vector<ViewColumn>& ColumnsColumns();
absl::StatusOr<std::vector<Row>> ColumnsRows(const ViewContext& context);

const std::vector<ViewColumn>& ColumnFieldPathsColumns();
absl::StatusOr<std::vector<Row>> ColumnFieldPathsRows(const ViewContext& context);

}  // namespace bigquery_emulator_duckdb::information_schema
