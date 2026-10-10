#include "src/interval.h"

#include <cstdint>

#include "absl/status/statusor.h"
#include "duckdb.h"

namespace bigquery_emulator_duckdb {

absl::StatusOr<googlesql::IntervalValue> IntervalFromDuckDb(const duckdb_interval& value) {
  return googlesql::IntervalValue::FromMonthsDaysMicros(value.months, value.days, value.micros);
}

duckdb_interval IntervalToDuckDb(const googlesql::IntervalValue& value) {
  return {
      .months = static_cast<int32_t>(value.get_months()),
      .days = static_cast<int32_t>(value.get_days()),
      .micros = value.get_micros(),
  };
}

}  // namespace bigquery_emulator_duckdb
