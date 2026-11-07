#include "src/partition_policy_history.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/str_join.h"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/partitioning.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {
namespace {

std::string Policies(const std::string& project) {
  return QuoteIdentifier(project) + ".main.emulator_partition_policy_history";
}

std::string PolicyAtClause(const std::string& project, int64_t storage_id,
                           int64_t evaluation_time_micros) {
  return std::format(
      " FROM {} WHERE storage_id = {} AND "
      "(valid_from <= {} OR (txid = txid_current() AND valid_from IS NULL)) "
      "ORDER BY valid_from DESC NULLS FIRST, txid DESC LIMIT 1",
      Policies(project), storage_id, evaluation_time_micros);
}

}  // namespace

std::string CreatePartitionPolicyHistoryStatement(const std::string& project) {
  return std::format(
      "CREATE TABLE IF NOT EXISTS {} (storage_id BIGINT NOT NULL, txid UBIGINT NOT NULL, "
      "valid_from BIGINT, expiration_ms BIGINT CHECK (expiration_ms > 0), "
      "PRIMARY KEY (storage_id, txid))",
      Policies(project));
}

std::string KeepPartitionPolicyStatement(const std::string& project, int64_t storage_id,
                                         std::optional<int64_t> expiration_ms) {
  if (expiration_ms && *expiration_ms <= 0) {
    throw ApiError::Invalid("TTL for interval partition must be a positive number.");
  }
  return std::format(
      "INSERT INTO {} VALUES ({}, txid_current(), NULL, {}) ON CONFLICT (storage_id, txid) "
      "DO UPDATE SET expiration_ms = excluded.expiration_ms, valid_from = NULL",
      Policies(project), storage_id, expiration_ms ? std::to_string(*expiration_ms) : "NULL");
}

std::string KeepPartitionPolicyStatement(const TableReference& table,
                                         std::optional<int64_t> expiration_ms) {
  if (expiration_ms && *expiration_ms <= 0) {
    throw ApiError::Invalid("TTL for interval partition must be a positive number.");
  }
  return std::format(
      "INSERT INTO {} SELECT table_oid, txid_current(), NULL, {} FROM {} WHERE type = 'TABLE' "
      "ON CONFLICT (storage_id, txid) DO UPDATE SET expiration_ms = excluded.expiration_ms, "
      "valid_from = NULL",
      Policies(table.project_id), expiration_ms ? std::to_string(*expiration_ms) : "NULL",
      RelationsQuery(table.project_id, table.dataset_id, table.table_id));
}

std::string CommitPartitionPoliciesStatement(const std::string& project) {
  return "UPDATE " + Policies(project) +
         " SET valid_from = epoch_us(get_current_timestamp()) WHERE txid = txid_current()";
}

std::string CommitPartitionPoliciesStatement(const std::string& project, int64_t commit_micros) {
  return std::format(
      "UPDATE {} SET valid_from = {} WHERE txid = txid_current() AND "
      "valid_from IS NULL",
      Policies(project), commit_micros);
}

std::string OpenPartitionPolicySnapshotsStatement(const std::vector<std::string>& projects) {
  std::vector<std::string> reads;
  reads.reserve(projects.size());
  std::ranges::transform(projects, std::back_inserter(reads), [](const std::string& project) {
    return "SELECT storage_id FROM " + Policies(project);
  });
  return "SELECT * FROM (" + absl::StrJoin(reads, " UNION ALL ") + ") LIMIT 0";
}

std::optional<int64_t> PartitionPolicyAt(Backend& backend, const std::string& project,
                                         int64_t storage_id, int64_t evaluation_time_micros) {
  const auto result = backend.Execute(
      "SELECT expiration_ms, CAST(valid_from IS NULL AS BIGINT), "
      "CAST(expiration_ms IS NULL AS BIGINT)" +
      PolicyAtClause(project, storage_id, evaluation_time_micros));
  if (result.rows.empty()) {
    return std::nullopt;
  }
  if (result.rows.Int64(0, 1) != 0) {
    throw ApiError::Unsupported("partition expiration policy visibility inside a transaction");
  }
  return result.rows.Int64(0, 2) != 0 ? std::nullopt : std::optional(result.rows.Int64(0, 0));
}

std::string PartitionPolicyReadQuery(const std::string& project, int64_t storage_id,
                                     const std::string& rows_query,
                                     const TimePartitioning& partitioning,
                                     const std::vector<FieldSchema>& schema,
                                     int64_t evaluation_time_micros,
                                     bool reject_natural_deadlines) {
  const std::string duration =
      "(SELECT CASE WHEN valid_from IS NULL THEN error(" +
      QuoteLiteral("Unsupported: partition expiration policy visibility inside a transaction") +
      ") ELSE expiration_ms END" + PolicyAtClause(project, storage_id, evaluation_time_micros) +
      ")";
  const std::string expired =
      ExpiredPartitionPredicate(partitioning, schema, duration, evaluation_time_micros);
  if (reject_natural_deadlines) {
    const std::string policy_time =
        "(SELECT valid_from" + PolicyAtClause(project, storage_id, evaluation_time_micros) + ")";
    const std::string expired_at_publication =
        ExpiredPartitionPredicate(partitioning, schema, duration, policy_time);
    return std::format(
        "SELECT * FROM ({}) AS _bq_partition_rows WHERE CASE WHEN ({}) AND NOT ({}) "
        "THEN error('Unsupported: partition expiration after natural deadline passage') "
        "ELSE NOT ({}) END",
        rows_query, expired, expired_at_publication, expired);
  }
  return "SELECT * FROM (" + rows_query + ") AS _bq_partition_rows WHERE NOT (" + expired + ")";
}

}  // namespace bigquery_emulator_duckdb
