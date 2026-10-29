#pragma once

#include <cstdint>

namespace bigquery_emulator_duckdb {

enum class TimestampPrecision : std::uint8_t { kMicroseconds, kNanoseconds };

// An alias, not a distinct DuckDB type. Resolved GoogleSQL types remain authoritative when
// expressions such as MIN or CASE return a HUGEINT without its alias.
inline constexpr const char* kTimestampNanosType = "bq_timestamp_nanos";

}  // namespace bigquery_emulator_duckdb
