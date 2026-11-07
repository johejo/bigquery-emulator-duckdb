#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "src/backend.h"
#include "src/field_schema.h"
#include "src/partitioning.h"

namespace bigquery_emulator_duckdb {

// Partition policies have their own history: changing a policy must not copy or delete rows.
// The identity is the DuckDB storage OID, not a name or a millisecond creation timestamp.
// This component stores committed policies only; it does not define policy visibility inside
// a script transaction, enable expiration requests, or purge physically retained rows.
std::string CreatePartitionPolicyHistoryStatement(const std::string& project);

// Stage the transaction's final policy for this identity. NULL is an explicit removal; no
// record means the table has never had expiration. Run in the policy publication transaction,
// after revalidating the table identity and policy under the runtime's write coordination.
std::string KeepPartitionPolicyStatement(const std::string& project, int64_t storage_id,
                                         std::optional<int64_t> expiration_ms);

// Finalize staged policies immediately before committing the publication transaction. The
// caller supplies one commit cutoff in microseconds, shared with the operation's other history.
// Multiple changes in one transaction publish only its final policy. Rollback publishes none.
std::string CommitPartitionPoliciesStatement(const std::string& project, int64_t commit_micros);

// The committed policy effective at evaluation_time_micros. At a change boundary the new
// policy applies. Reads do not create history, and later changes cannot rewrite earlier reads.
// Reading a staged policy in its own transaction is deliberately unsupported until transaction
// visibility is independently established; committed policies in other sessions remain readable.
std::optional<int64_t> PartitionPolicyAt(Backend& backend, const std::string& project,
                                         int64_t storage_id, int64_t evaluation_time_micros);

// Filter a raw current or historical row query using the policy effective at the same cutoff.
// Unlike fetching PartitionPolicyAt before executing the row query, this reads both in one
// execution snapshot: a policy committed between query construction and execution is included.
// The caller must supply the corresponding row version and revalidate its storage identity;
// this helper does not resolve row history, coordinate writes, or define natural deadline passage.
// An uncommitted policy in the executing transaction is rejected, as in PartitionPolicyAt.
std::string PartitionPolicyReadQuery(const std::string& project, int64_t storage_id,
                                     const std::string& rows_query,
                                     const TimePartitioning& partitioning,
                                     const std::vector<FieldSchema>& schema,
                                     int64_t evaluation_time_micros);

}  // namespace bigquery_emulator_duckdb
