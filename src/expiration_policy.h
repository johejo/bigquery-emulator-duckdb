#pragma once

#include <cstdint>
#include <optional>

namespace bigquery_emulator_duckdb {

// Effective policy for an ordinary new native table, not a copy, clone, snapshot, LIKE or
// replacement. Durations and the absolute table deadline use REST's milliseconds.
struct ExpirationPolicy {
  std::optional<int64_t> table_deadline_ms = std::nullopt;
  std::optional<int64_t> partition_duration_ms = std::nullopt;

  bool operator==(const ExpirationPolicy&) const = default;
};

// Resolve request-layer inputs before persisting metadata. Omission and explicit NULL both
// inherit on ordinary creation (unlike PATCH/ALTER removal). Zero is not a removal spelling.
// A dataset's partition default suppresses its table default only for time-partitioned tables,
// even when the request supplies its own partition duration. Explicit table deadlines survive.
// This does not enable partition expiration at any runtime entry point.
ExpirationPolicy ResolveCreationExpiration(bool time_partitioned, int64_t creation_time_ms,
                                           ExpirationPolicy requested,
                                           std::optional<int64_t> default_table_duration_ms,
                                           std::optional<int64_t> default_partition_duration_ms);

}  // namespace bigquery_emulator_duckdb
