#include "src/table_history.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/duckdb_sql.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {
namespace {

std::string HistoryTable(const std::string& project) {
  return QuoteIdentifier(project) + ".main.emulator_table_history";
}

// The versions of `table`. DuckDB names relations without regard to ASCII case, as the history
// does.
std::string VersionsOf(const TableReference& table, int64_t creation_time) {
  return std::format("dataset_id = {} AND table_id = {} AND creation_time = {}",
                     QuoteLiteral(ToLowerAscii(table.dataset_id)),
                     QuoteLiteral(ToLowerAscii(table.table_id)), creation_time);
}

// The name of the table in the project's main schema that holds the rows of each version of
// `table`.
std::string VersionRowsName(const TableReference& table, int64_t creation_time) {
  return QuoteIdentifier(std::format("emulator_history.{}.{}.{}", ToLowerAscii(table.dataset_id),
                                     ToLowerAscii(table.table_id), creation_time));
}

std::string VersionRows(const TableReference& table, int64_t creation_time) {
  return QuoteIdentifier(table.project_id) + ".main." + VersionRowsName(table, creation_time);
}

// The next version of `table`, as a query of one row, which has none once the transaction kept
// one, since it keeps the rows the table had before its first write.
std::string NextVersion(const TableReference& table, int64_t creation_time) {
  return std::format(
      "SELECT coalesce(max(version), 0) + 1 AS version FROM {} WHERE {} HAVING NOT "
      "coalesce(bool_or(txid = txid_current() AND valid_to >= epoch_us(transaction_timestamp())), "
      "false)",
      HistoryTable(table.project_id), VersionsOf(table, creation_time));
}

}  // namespace

std::vector<std::string> CreateHistoryStatements(const std::string& project) {
  // A row without a dataset dates the start of the history.
  return {
      std::format("CREATE TABLE IF NOT EXISTS {} (dataset_id VARCHAR, table_id VARCHAR, "
                  "creation_time BIGINT, version BIGINT, valid_to BIGINT, kept BOOLEAN, "
                  "txid UBIGINT)",
                  HistoryTable(project)),
      std::format(
          "INSERT INTO {0} SELECT NULL, NULL, NULL, NULL, epoch_us(get_current_timestamp()), "
          "false, NULL WHERE NOT EXISTS (SELECT 1 FROM {0} WHERE dataset_id IS NULL)",
          HistoryTable(project)),
  };
}

std::vector<std::string> KeepVersionStatements(const TableReference& table, int64_t creation_time) {
  const std::string history = HistoryTable(table.project_id);
  const std::string versions = VersionsOf(table, creation_time);
  const std::string rows = VersionRows(table, creation_time);
  const std::string expired = std::format("valid_to < epoch_us(get_current_timestamp()) - {}",
                                          kTimeTravelWindowMillis * 1000);
  return {
      std::format(
          "CREATE TABLE IF NOT EXISTS {} AS SELECT CAST(NULL AS BIGINT) AS _TABLE_VERSION, * "
          "FROM {} WITH NO DATA",
          rows, QualifiedName(table)),
      std::format("INSERT INTO {} SELECT v.version, t.* FROM {} AS t, ({}) AS v", rows,
                  QualifiedName(table), NextVersion(table, creation_time)),
      std::format("INSERT INTO {} SELECT {}, {}, {}, version, epoch_us(get_current_timestamp()), "
                  "true, txid_current() FROM ({})",
                  history, QuoteLiteral(ToLowerAscii(table.dataset_id)),
                  QuoteLiteral(ToLowerAscii(table.table_id)), creation_time,
                  NextVersion(table, creation_time)),
      std::format("DELETE FROM {} WHERE _TABLE_VERSION IN (SELECT version FROM {} WHERE {} AND {})",
                  rows, history, versions, expired),
      std::format("DELETE FROM {} WHERE {} AND {}", history, versions, expired),
  };
}

std::string CommitVersionsStatement(const std::string& project) {
  return std::format(
      "UPDATE {} SET valid_to = epoch_us(get_current_timestamp()) WHERE txid = txid_current() "
      "AND valid_to >= epoch_us(transaction_timestamp())",
      HistoryTable(project));
}

std::vector<std::string> DiscardHistoryStatements(const TableReference& table,
                                                  int64_t creation_time) {
  return {
      std::format("INSERT INTO {} SELECT {}, {}, {}, coalesce(max(version), 0) + 1, "
                  "epoch_us(get_current_timestamp()), false, txid_current() FROM {} WHERE {}",
                  HistoryTable(table.project_id), QuoteLiteral(ToLowerAscii(table.dataset_id)),
                  QuoteLiteral(ToLowerAscii(table.table_id)), creation_time,
                  HistoryTable(table.project_id), VersionsOf(table, creation_time)),
      "DROP TABLE IF EXISTS " + VersionRows(table, creation_time),
  };
}

std::vector<std::string> RenameHistoryStatements(const TableReference& table, int64_t creation_time,
                                                 const std::string& table_id) {
  TableReference renamed = table;
  renamed.table_id = table_id;
  return {
      std::format("UPDATE {} SET table_id = {} WHERE {}", HistoryTable(table.project_id),
                  QuoteLiteral(ToLowerAscii(table_id)), VersionsOf(table, creation_time)),
      std::format("ALTER TABLE IF EXISTS {} RENAME TO {}", VersionRows(table, creation_time),
                  VersionRowsName(renamed, creation_time)),
  };
}

void SweepHistory(Backend& backend, const std::string& project) {
  // The history of each table the project has now, as VersionRowsName and VersionsOf name it.
  const std::string tables = std::format(
      "SELECT lower(schema_name) AS dataset_id, lower(table_name) AS table_id, "
      "TRY_CAST(CASE WHEN json_valid(comment) THEN json_extract_string(comment, '$.creationTime') "
      "END AS BIGINT) AS creation_time FROM duckdb_tables() WHERE database_name = {} AND "
      "schema_name <> 'main'",
      QuoteLiteral(project));
  try {
    const QueryResult forgotten = backend.Execute(std::format(
        "SELECT table_name FROM duckdb_tables() WHERE database_name = {} AND schema_name = 'main' "
        "AND starts_with(table_name, 'emulator_history.') AND table_name NOT IN (SELECT "
        "'emulator_history.' || dataset_id || '.' || table_id || '.' || creation_time FROM ({}) "
        "WHERE creation_time IS NOT NULL)",
        QuoteLiteral(project), tables));
    for (const auto& row : forgotten.rows) {
      backend.Execute(std::format("DROP TABLE IF EXISTS {}.main.{}", QuoteIdentifier(project),
                                  QuoteIdentifier(row.at("f").at(0).at("v").get<std::string>())));
    }
    backend.Execute(std::format(
        "DELETE FROM {} AS h WHERE dataset_id IS NOT NULL AND NOT EXISTS (SELECT 1 FROM ({}) AS t "
        "WHERE t.dataset_id = h.dataset_id AND t.table_id = h.table_id AND t.creation_time = "
        "h.creation_time)",
        HistoryTable(project), tables));
    // NOLINTNEXTLINE(bugprone-empty-catch): the next sweep drops what a conflict left.
  } catch (const BackendError&) {
  }
}

absl::StatusOr<std::string> TimeTravelQuery(Backend& backend, const TableReference& table,
                                            int64_t creation_time, int64_t time) {
  if (creation_time == 0) {
    return absl::UnimplementedError("FOR SYSTEM_TIME AS OF a table without a creation time");
  }
  const int64_t now = absl::ToUnixMicros(absl::Now());
  if (time > now) {
    return absl::InvalidArgumentError(std::format(
        "Query error: 'FOR SYSTEM_TIME AS OF' expression for table '{}.{}.{}' evaluates to a "
        "TIMESTAMP value in the future: {}.",
        table.project_id, table.dataset_id, table.table_id,
        absl::FormatTime("%Y-%m-%d %H:%M:%E3S UTC", absl::FromUnixMicros(time),
                         absl::UTCTimeZone())));
  }
  // BigQuery reads back to the table's creation within its time travel window.
  const int64_t millis = time < 0 ? (time - 999) / 1000 : time / 1000;
  if (const int64_t earliest = (now / 1000) - kTimeTravelWindowMillis; millis < earliest) {
    return absl::InvalidArgumentError(
        std::format("Invalid value: Invalid time travel timestamp {0} for table {1}@{0}. Cannot "
                    "read before {2}",
                    millis, TableName(table), std::max(earliest, creation_time)));
  }
  // BigQuery reads a table that the table replaced, which the emulator does not keep.
  if (time < creation_time * 1000) {
    return absl::UnimplementedError("FOR SYSTEM_TIME AS OF a time before the table was created");
  }
  const std::string history = HistoryTable(table.project_id);
  const std::string versions = VersionsOf(table, creation_time);
  // The version that lasted past `time`: the first such kept version, or the table's own rows.
  const QueryResult later = backend.Execute(std::format(
      "SELECT count(*) FILTER (WHERE NOT kept), count(*) FROM {} WHERE ({} OR dataset_id IS NULL) "
      "AND valid_to > {}",
      history, versions, time));
  const nlohmann::json cells = later.rows.at(0).at("f");
  if (cells.at(0).at("v") != "0") {
    return absl::UnimplementedError(
        "FOR SYSTEM_TIME AS OF a time before the table's schema changed or it was renamed");
  }
  if (cells.at(1).at("v") == "0") {
    return "SELECT * FROM " + QualifiedName(table);
  }
  // Selecting the version as the query runs keeps the result right if a write commits meanwhile.
  const std::string kept =
      std::format("SELECT 1 FROM {} WHERE {} AND valid_to > {}", history, versions, time);
  return std::format(
      "SELECT * FROM {} WHERE NOT EXISTS ({}) UNION ALL SELECT * EXCLUDE (_TABLE_VERSION) FROM {} "
      "WHERE "
      "_TABLE_VERSION = (SELECT min(version) FROM {} WHERE {} AND valid_to > {})",
      QualifiedName(table), kept, VersionRows(table, creation_time), history, versions, time);
}

}  // namespace bigquery_emulator_duckdb
