#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bigquery_emulator_duckdb {

enum class FileFormat : uint8_t { kCsv, kJson, kParquet };
enum class DecimalTargetType : uint8_t { kNumeric, kBigNumeric, kString };

// Reader settings only. Callers validate their own load or external-table options before
// mapping them to these settings; destination and schema-update policies do not belong here.
struct CsvReadOptions {
  int skip_leading_rows = 0;
  std::string field_delimiter = ",";
  // None when values are never quoted.
  std::optional<char> quote = '"';
  bool allow_quoted_newlines = false;
  bool allow_jagged_rows = false;
  bool ignore_unknown_values = false;
  // nullMarker or nullMarkers, which replace the empty value as NULL; at most one is set.
  std::optional<std::string> null_marker = std::nullopt;
  std::optional<std::vector<std::string>> null_markers = std::nullopt;
  bool preserve_ascii_control_characters = false;
  // Whether a quoted empty value is NULL, as an unquoted one is, rather than an empty STRING,
  // when no NULL marker is set.
  bool quoted_nulls = true;
  // Whether staged records begin with their number, which FileReadQuery reads as the column
  // "bad.record", and FileReadQuery leaves out a record with a value its reader cannot convert,
  // which the caller finds and counts as bad, rather than failing.
  bool numbered_records = false;
};

// A record a read skips as bad, of which BigQuery warns.
struct BadRecord {
  // Its number among the records of the read, counting from 1.
  int64_t number = 0;
  std::string uri = {};
  // BigQuery's message of the record, without the File it names.
  std::string message = {};
  // Whether BigQuery counts it among the Rows of its message of an exceeded limit, which a CSV
  // record of too many values is not.
  bool row = true;
};

// Where a CSV record of a read starts.
struct RecordPosition {
  size_t line = 0;
  size_t offset = 0;
};

// The bad records a read skips, at most `limit`, its maxBadRecords. `records` numbers the records
// the read has reached, bad ones included. With `stops`, staging fails at the record that exceeds
// the limit; otherwise it skips every bad record, and the caller enforces the limit once it has
// found them all.
struct BadRecords {
  int64_t limit = 0;
  int64_t records = 0;
  bool stops = false;
  std::vector<BadRecord> found = {};
  // Of each CSV record, by number, which StageCsvSources records.
  std::vector<RecordPosition> positions = {};

  // Adds `record`, and whether the read may skip it.
  bool Skip(BadRecord record) {
    found.push_back(std::move(record));
    return !stops || std::cmp_less_equal(found.size(), limit);
  }
};
struct JsonReadOptions {
  // External reads use the declared types, so inference cannot alter STRING values.
  bool explicit_schema = false;
};
struct ParquetReadOptions {
  // External tables allow files to omit nullable columns of the table schema.
  bool allow_missing_columns = false;
  // External reads preserve NULL LIST elements; loads reject them.
  bool allow_null_list_elements = false;
};
using FileReadOptions = std::variant<CsvReadOptions, JsonReadOptions, ParquetReadOptions>;

}  // namespace bigquery_emulator_duckdb
