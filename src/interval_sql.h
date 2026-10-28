#pragma once

#include <string>

#include "googlesql/public/interval_value.h"

namespace bigquery_emulator_duckdb {

// The DuckDB expression for `value`, which keeps its month, day and microsecond parts apart.
std::string IntervalSql(const googlesql::IntervalValue& value);

}  // namespace bigquery_emulator_duckdb
