#include "src/expiration_policy.h"

#include <cstdint>
#include <limits>
#include <optional>

#include "src/api_error.h"

namespace bigquery_emulator_duckdb {
namespace {

void ValidatePartitionDuration(std::optional<int64_t> duration) {
  if (duration && *duration <= 0) {
    throw ApiError::Invalid("TTL for interval partition must be a positive number.");
  }
}

}  // namespace

ExpirationPolicy ResolveCreationExpiration(bool time_partitioned, int64_t creation_time_ms,
                                           ExpirationPolicy requested,
                                           std::optional<int64_t> default_table_duration_ms,
                                           std::optional<int64_t> default_partition_duration_ms) {
  ValidatePartitionDuration(requested.partition_duration_ms);
  ValidatePartitionDuration(default_partition_duration_ms);
  if (!time_partitioned && requested.partition_duration_ms) {
    throw ApiError::Invalid("Partition expiration requires time partitioning");
  }
  if (time_partitioned) {
    if (!requested.partition_duration_ms) {
      requested.partition_duration_ms = default_partition_duration_ms;
    }
    // Suppression is determined by the dataset default, not the effective table policy.
    if (default_partition_duration_ms) {
      default_table_duration_ms.reset();
    }
  }
  if (!requested.table_deadline_ms && default_table_duration_ms) {
    const int64_t duration = *default_table_duration_ms;
    if ((duration > 0 && creation_time_ms > std::numeric_limits<int64_t>::max() - duration) ||
        (duration < 0 && creation_time_ms < std::numeric_limits<int64_t>::min() - duration)) {
      throw ApiError::Invalid("The default table expiration of the dataset is out of range");
    }
    requested.table_deadline_ms = creation_time_ms + duration;
  }
  return requested;
}

}  // namespace bigquery_emulator_duckdb
