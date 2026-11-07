#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

enum class TimePartitionUnit : uint8_t { kDay, kHour, kMonth, kYear };
std::string_view TimePartitionUnitName(TimePartitionUnit unit);
std::optional<TimePartitionUnit> ParseTimePartitionUnit(std::string_view name);

// Table.timePartitioning on a column. Ingestion-time partitioning, which adds pseudo-columns,
// and partition expiration are not yet exposed by the runtime.
struct TimePartitioning {
  TimePartitionUnit type = TimePartitionUnit::kDay;
  std::string field;

  bool operator==(const TimePartitioning&) const = default;
};

// Validates the column and unit and returns the column with its schema spelling.
// Throws ApiError when the column cannot be used for time-unit partitioning.
const FieldSchema& TimePartitionColumn(const TimePartitioning& partitioning,
                                       const std::vector<FieldSchema>& schema);

// A DuckDB predicate over the partition column, true only for ordinary partitions whose UTC
// boundary plus duration is at or before evaluation_time_micros. NULL and out-of-range values
// are special partitions, not ordinary dated partitions. DATETIME is a civil value, not a
// session-local instant. The explicit cutoff allows deterministic boundary tests and lets a
// caller reuse one evaluation time across an operation. This does not perform cleanup or
// establish the policy to use for a historical read.
std::string ExpiredPartitionPredicate(const TimePartitioning& partitioning,
                                      const std::vector<FieldSchema>& schema, int64_t duration_ms,
                                      int64_t evaluation_time_micros);

// The duration comes from a trusted SQL expression rather than a constant. Its non-NULL values
// must already be validated as positive milliseconds; NULL means no expiration. This lets row
// and policy history be read in the same DuckDB snapshot, without fetching a policy separately.
std::string ExpiredPartitionPredicate(const TimePartitioning& partitioning,
                                      const std::vector<FieldSchema>& schema,
                                      std::string_view duration_ms_sql,
                                      int64_t evaluation_time_micros);

}  // namespace bigquery_emulator_duckdb
