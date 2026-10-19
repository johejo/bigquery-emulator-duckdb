#pragma once

#include <vector>

#include "absl/status/statusor.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"

namespace bigquery_emulator_duckdb::information_schema {

const std::vector<ViewColumn>& RoutinesColumns();
absl::StatusOr<std::vector<Row>> RoutinesRows(const ViewContext& context);

const std::vector<ViewColumn>& ParametersColumns();
absl::StatusOr<std::vector<Row>> ParametersRows(const ViewContext& context);

}  // namespace bigquery_emulator_duckdb::information_schema
