#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace bigquery_emulator_duckdb {

enum class FileFormat : uint8_t { kCsv, kJson, kParquet };
enum class DecimalTargetType : uint8_t { kNumeric, kBigNumeric, kString };

// Reader settings only. Callers validate their own load or external-table options before
// mapping them to these settings; destination and schema-update policies do not belong here.
struct CsvReadOptions {
  int skip_leading_rows = 0;
  std::string field_delimiter = ",";
  // Whether a quoted empty value is NULL, as an unquoted one is, rather than an empty STRING.
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
