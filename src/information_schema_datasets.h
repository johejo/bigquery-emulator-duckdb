#pragma once

#include <vector>

#include "absl/status/statusor.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"

namespace bigquery_emulator_duckdb::information_schema {

const std::vector<ViewColumn>& SchemataColumns();
absl::StatusOr<std::vector<Row>> SchemataRows(const ViewContext& context);

const std::vector<ViewColumn>& SchemataOptionsColumns();
absl::StatusOr<std::vector<Row>> SchemataOptionsRows(const ViewContext& context);

}  // namespace bigquery_emulator_duckdb::information_schema
