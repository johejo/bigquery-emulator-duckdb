#include "src/partition_policy_history.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string>

#include "src/api_error.h"
#include "src/backend.h"
#include "src/duckdb_sql.h"

namespace bigquery_emulator_duckdb {
namespace {

std::string Policies(const std::string& project) {
  return QuoteIdentifier(project) + ".main.emulator_partition_policy_history";
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

std::string CommitPartitionPoliciesStatement(const std::string& project, int64_t commit_micros) {
  return std::format(
      "UPDATE {} SET valid_from = {} WHERE txid = txid_current() AND "
      "valid_from IS NULL",
      Policies(project), commit_micros);
}

std::optional<int64_t> PartitionPolicyAt(Backend& backend, const std::string& project,
                                         int64_t storage_id, int64_t evaluation_time_micros) {
  const auto result = backend.Execute(
      std::format("SELECT expiration_ms, CAST(valid_from IS NULL AS BIGINT), "
                  "CAST(expiration_ms IS NULL AS BIGINT) FROM {} WHERE storage_id = {} AND "
                  "(valid_from <= {} OR (txid = txid_current() AND valid_from IS NULL)) "
                  "ORDER BY valid_from DESC NULLS FIRST, txid DESC LIMIT 1",
                  Policies(project), storage_id, evaluation_time_micros));
  if (result.rows.empty()) {
    return std::nullopt;
  }
  if (result.rows.Int64(0, 1) != 0) {
    throw ApiError::Unsupported("partition expiration policy visibility inside a transaction");
  }
  return result.rows.Int64(0, 2) != 0 ? std::nullopt : std::optional(result.rows.Int64(0, 0));
}

}  // namespace bigquery_emulator_duckdb
