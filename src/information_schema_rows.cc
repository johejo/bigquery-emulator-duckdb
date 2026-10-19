#include "src/information_schema_rows.h"

#include <cstdint>
#include <string>

#include "absl/time/time.h"
#include "src/duckdb_sql.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb::information_schema {

Cell String(const std::string& value) { return QuoteLiteral(value); }
Cell Int(int64_t value) { return std::to_string(value); }

// A time in milliseconds since the Unix epoch as INFORMATION_SCHEMA spells it.
std::string FormatMillis(int64_t millis) {
  return absl::FormatTime("%Y-%m-%dT%H:%M:%E3SZ", absl::FromUnixMillis(millis),
                          absl::UTCTimeZone());
}

// The time a dataset, table or view was created, or NULL for one created before the emulator
// recorded it.
Cell CreationTime(int64_t millis) {
  if (millis == 0) {
    return kNull;
  }
  return String(FormatMillis(millis));
}

// An unset description, default or rounding mode, which the table resource omits, is NULL.
Cell OptionalString(const std::string& value) { return value.empty() ? kNull : String(value); }

// The cells that name a dataset or a table in a row: its project, dataset and, for a table, its
// name.
Row Identity(const DatasetReference& dataset) {
  return {String(dataset.project_id), String(dataset.dataset_id)};
}

Row Identity(const DatasetReference& dataset, const std::string& table) {
  return {String(dataset.project_id), String(dataset.dataset_id), String(table)};
}

// `identity` followed by `cells`.
Row Concat(Row identity, const Row& cells) {
  identity.insert(identity.end(), cells.begin(), cells.end());
  return identity;
}

}  // namespace bigquery_emulator_duckdb::information_schema
