#pragma once

namespace bigquery_emulator_duckdb::functions_duckdb {

// Which of BigQuery's differences from GoogleSQL the backend functions adapt. Defaults keep
// GoogleSQL's behavior; each registration passes its function only the choices it reads.
struct Options {
  // CHR(0) is an empty string.
  bool chr_zero_is_empty = false;
  // FORMAT's integer precision and grouping, and %t ending at NUL.
  bool format_specifiers = false;
  // FORMAT writes no sign for a NaN.
  bool unsigned_nan = false;
  // CAST(FLOAT64 AS STRING) and FORMAT write the sign of a negative zero.
  bool signed_zero_strings = false;
  // FORMAT_DATE, FORMAT_DATETIME and FORMAT_TIMESTAMP do not pad %G below 1000.
  bool unpadded_iso_year = false;
  // GENERATE_ARRAY and GENERATE_DATE_ARRAY exceed GoogleSQL's 16,000-element limit.
  bool extended_array_limits = false;
  // FLOAT64 TRUNC returns positive zero at its early zero exits.
  bool trunc_early_positive_zero = false;
  // JSON_ARRAY_APPEND and JSON_ARRAY_INSERT reject numbers they would round, and keep the sign
  // of a negative zero.
  bool json_strict_numbers = false;
  bool json_preserve_negative_zero = false;
};

}  // namespace bigquery_emulator_duckdb::functions_duckdb
