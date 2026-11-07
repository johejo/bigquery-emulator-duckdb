#include "src/partition_policy_read.h"

#include <cstdint>
#include <format>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "src/backend.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/partition_policy_history.h"
#include "src/references.h"
#include "src/table_comments.h"
#include "src/table_history.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

absl::StatusOr<std::string> PartitionPolicyTableReadQuery(Backend& backend,
                                                          const TableReference& table,
                                                          int64_t evaluation_time_micros,
                                                          PartitionRowVersion version,
                                                          bool reject_natural_deadlines) {
  if (!backend.InTransaction()) {
    return absl::FailedPreconditionError("partition policy reads require an execution transaction");
  }
  const std::string identity = "SELECT table_oid FROM " +
                               RelationsQuery(table.project_id, table.dataset_id, table.table_id) +
                               " WHERE type = 'TABLE'";
  const auto stored = backend.Execute(identity);
  if (stored.rows.empty()) {
    return absl::NotFoundError("Not found: Table " + TableName(table));
  }
  const int64_t storage_id = stored.rows.Int64(0, 0);
  const auto comment = RelationComment(backend, RelationType::kTable, table);
  const TableMetadata metadata = comment ? CommentMetadata(*comment) : TableMetadata{};
  if (!metadata.time_partitioning || metadata.external_data || metadata.snapshot ||
      metadata.clone) {
    return absl::UnimplementedError("partition policy reads of non-native time-partitioned tables");
  }
  std::string rows = "SELECT * FROM " + QualifiedName(table);
  if (version == PartitionRowVersion::kHistorical) {
    auto historical =
        TimeTravelQuery(backend, table, metadata.creation_time, evaluation_time_micros);
    if (!historical.ok()) {
      return historical.status();
    }
    rows = *historical;
  }
  const std::string visible = PartitionPolicyReadQuery(
      table.project_id, storage_id, rows, *metadata.time_partitioning, TableSchema(backend, table),
      evaluation_time_micros, reject_natural_deadlines);
  // Keep the check in the query rather than checking the OID only during construction. It must
  // observe the same snapshot as the selected raw row version and its policy.
  return "SELECT * FROM (" + visible + ") AS _bq_visible_partition_rows WHERE CASE WHEN (" +
         identity + ") IS NOT DISTINCT FROM " + std::to_string(storage_id) + " THEN TRUE ELSE " +
         "error(" +
         QuoteLiteral("Table changed during partition policy read: " + TableName(table)) + ") END";
}

}  // namespace bigquery_emulator_duckdb
