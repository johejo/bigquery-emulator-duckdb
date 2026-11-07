#pragma once

#include <cstdint>
#include <string>

#include "absl/status/statusor.h"
#include "src/backend.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {

// Which raw row version to read before applying the policy at the supplied cutoff.
// Historical reads never start from an already filtered current read.
enum class PartitionRowVersion : uint8_t { kCurrent, kHistorical };

// Build a read of a native time-unit partitioned table. Resolve raw row history and policy
// history together, and check the storage identity again in the executing snapshot. A table
// replaced after construction must not be read with its predecessor's policy or row history.
// The caller supplies one operation cutoff, and must build and execute in the same active
// transaction on the supplied backend. Construction without a transaction is rejected: row
// history resolution must not race a write that creates the first retained row version.
// This does not enable expiration requests, define natural-deadline time travel or transaction
// policy visibility, or coordinate mutations. Retained rows are never deleted by a read.
absl::StatusOr<std::string> PartitionPolicyTableReadQuery(Backend& backend,
                                                          const TableReference& table,
                                                          int64_t evaluation_time_micros,
                                                          PartitionRowVersion version);

}  // namespace bigquery_emulator_duckdb
