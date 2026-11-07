#include "src/partitioning.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

std::string_view TimePartitionUnitName(TimePartitionUnit unit) {
  switch (unit) {
    case TimePartitionUnit::kDay:
      return "DAY";
    case TimePartitionUnit::kHour:
      return "HOUR";
    case TimePartitionUnit::kMonth:
      return "MONTH";
    case TimePartitionUnit::kYear:
      return "YEAR";
  }
  return "";
}

std::optional<TimePartitionUnit> ParseTimePartitionUnit(std::string_view name) {
  for (const auto unit : {
           TimePartitionUnit::kDay,
           TimePartitionUnit::kHour,
           TimePartitionUnit::kMonth,
           TimePartitionUnit::kYear,
       }) {
    if (name == TimePartitionUnitName(unit)) {
      return unit;
    }
  }
  return std::nullopt;
}

const FieldSchema& TimePartitionColumn(const TimePartitioning& partitioning,
                                       const std::vector<FieldSchema>& schema) {
  const auto field = std::ranges::find_if(schema, [&](const FieldSchema& candidate) {
    return ToLowerAscii(candidate.name) == ToLowerAscii(partitioning.field);
  });
  if (field == schema.end()) {
    throw ApiError::Invalid("The field specified for partitioning cannot be found in the schema: " +
                            partitioning.field);
  }
  const bool time = field->type == FieldType::kTimestamp || field->type == FieldType::kDatetime;
  if ((!time && field->type != FieldType::kDate) || field->mode == FieldMode::kRepeated) {
    throw ApiError::Invalid(std::format(
        "The field specified for time partitioning can only be of type TIMESTAMP, DATE or "
        "DATETIME. The type found is: {}.",
        FieldTypeName(field->type)));
  }
  if (!time && partitioning.type == TimePartitionUnit::kHour) {
    throw ApiError::Invalid("A DATE field cannot be partitioned by HOUR: " + field->name);
  }
  return *field;
}

std::string ExpiredPartitionPredicate(const TimePartitioning& partitioning,
                                      const std::vector<FieldSchema>& schema, int64_t duration_ms,
                                      int64_t evaluation_time_micros) {
  if (duration_ms <= 0) {
    throw ApiError::Invalid("TTL for interval partition must be a positive number.");
  }
  return ExpiredPartitionPredicate(partitioning, schema, std::to_string(duration_ms),
                                   evaluation_time_micros);
}

std::string ExpiredPartitionPredicate(const TimePartitioning& partitioning,
                                      const std::vector<FieldSchema>& schema,
                                      std::string_view duration_ms_sql,
                                      int64_t evaluation_time_micros) {
  return ExpiredPartitionPredicate(partitioning, schema, duration_ms_sql,
                                   std::to_string(evaluation_time_micros));
}

std::string ExpiredPartitionPredicate(const TimePartitioning& partitioning,
                                      const std::vector<FieldSchema>& schema,
                                      std::string_view duration_ms_sql,
                                      std::string_view evaluation_time_micros_sql) {
  const auto& field = TimePartitionColumn(partitioning, schema);
  const std::string column = QuoteIdentifier(field.name);
  // TIMESTAMPTZ truncation uses the session timezone unless converted to UTC first. DATE and
  // DATETIME instead cast directly to a civil TIMESTAMP, with no timezone conversion.
  const std::string civil = field.type == FieldType::kTimestamp
                                ? "timezone('UTC', " + column + ")"
                                : "CAST(" + column + " AS TIMESTAMP)";
  // Widen before either the addition or the milliseconds-to-microseconds multiplication:
  // even a valid positive int64 duration can overflow int64 microseconds.
  return std::format(
      "coalesce(({0}) >= TIMESTAMP '1960-01-01' AND ({0}) < TIMESTAMP '2160-01-01' AND "
      "CAST(epoch_us(date_trunc({1}, {0})) AS HUGEINT) + CAST(({2}) AS HUGEINT) * 1000 <= "
      "CAST({3} AS HUGEINT), false)",
      civil, QuoteLiteral(TimePartitionUnitName(partitioning.type)), duration_ms_sql,
      evaluation_time_micros_sql);
}

}  // namespace bigquery_emulator_duckdb
