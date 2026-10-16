#include "src/interval.h"

#include <cstdint>
#include <string>

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

std::string IntervalSql(const googlesql::IntervalValue& value) {
  return "bq_interval_parts(" + std::to_string(value.get_months()) + ", " +
         std::to_string(value.get_days()) + ", " + std::to_string(value.get_micros()) + ")";
}

}  // namespace bigquery_emulator_duckdb
