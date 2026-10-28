#pragma once

#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/public/interval_value.h"

namespace bigquery_emulator_duckdb {

// Keep months, days and time separate: EXTRACT and JUSTIFY observe those components.
absl::StatusOr<googlesql::IntervalValue> IntervalFromDuckDb(const duckdb_interval& value);
// The input has already been validated in microsecond mode.
duckdb_interval IntervalToDuckDb(const googlesql::IntervalValue& value);

}  // namespace bigquery_emulator_duckdb
