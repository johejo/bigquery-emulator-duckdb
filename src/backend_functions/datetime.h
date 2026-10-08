#pragma once

#include <cstdint>

#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/public/civil_time.h"

namespace bigquery_emulator_duckdb::backend_functions {

// DuckDB keeps a TIME, DATETIME or TIMESTAMP as microseconds since midnight or the epoch, a
// DATETIME as the civil time in UTC.
googlesql::TimeValue TimeFromMicros(int64_t micros);
int64_t MicrosFromTime(const googlesql::TimeValue& time);
absl::StatusOr<googlesql::DatetimeValue> DatetimeFromMicros(int64_t micros);
absl::StatusOr<int64_t> MicrosFromDatetime(const googlesql::DatetimeValue& datetime);

void RegisterDatetimeFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::backend_functions
