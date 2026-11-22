#include "src/interval_sql.h"

#include <string>

#include "googlesql/public/interval_value.h"

namespace bigquery_emulator_duckdb {

std::string IntervalSql(const googlesql::IntervalValue& value) {
  const std::string parts = std::to_string(value.get_months()) + ", " +
                            std::to_string(value.get_days()) + ", " +
                            std::to_string(value.get_micros());
  if (value.get_nano_fractions() == 0) {
    return "bq_interval_parts(" + parts + ")";
  }
  return "bq_interval_parts_nanos(" + parts + ", " + std::to_string(value.get_nano_fractions()) +
         ")";
}

}  // namespace bigquery_emulator_duckdb
