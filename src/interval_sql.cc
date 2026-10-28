#include "src/interval_sql.h"

#include <string>

#include "googlesql/public/interval_value.h"

namespace bigquery_emulator_duckdb {

std::string IntervalSql(const googlesql::IntervalValue& value) {
  return "bq_interval_parts(" + std::to_string(value.get_months()) + ", " +
         std::to_string(value.get_days()) + ", " + std::to_string(value.get_micros()) + ")";
}

}  // namespace bigquery_emulator_duckdb
