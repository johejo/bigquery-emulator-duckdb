#pragma once

#include <cstdint>
#include <optional>
#include <string>
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
