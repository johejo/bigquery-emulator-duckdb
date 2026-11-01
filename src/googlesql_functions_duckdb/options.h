#pragma once

#include <compare>

#include "src/googlesql_collation/options.h"
#include "src/timestamp_precision.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

// Which of BigQuery's differences from GoogleSQL the functions and their translations adapt.
// Defaults keep GoogleSQL's behavior; each registration reads only the choices it needs.
struct Options {
  TimestampPrecision timestamp_precision = TimestampPrecision::kNanoseconds;
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
  // FLOAT64 ROUND and TRUNC with digits return positive zero at their early zero exits.
  bool rounding_early_positive_zero = false;
  // JSON_ARRAY_APPEND and JSON_ARRAY_INSERT reject numbers they would round, and keep the sign
  // of a negative zero.
  bool json_strict_numbers = false;
  bool json_preserve_negative_zero = false;
  // JSON_KEYS rejects the SAFE. prefix.
  bool json_keys_rejects_safe = false;
  // The translator restricts COLLATE specifications to und:ci and the empty specification.
  bool collate_und_ci_only = false;
  // The translator rejects INTERSECT ALL and EXCEPT ALL.
  bool intersect_except_distinct_only = false;
  // Window PARTITION BY rejects STRUCT and ARRAY keys.
  bool scalar_window_partition_keys_only = false;
  // DISTINCT set operations reject STRUCT columns.
  bool distinct_set_operations_without_structs = false;
  // IS DISTINCT FROM and IS NOT DISTINCT FROM reject STRUCT arguments.
  bool distinct_comparisons_without_structs = false;
  // Query results and written values fail on an ARRAY with a NULL element.
  bool arrays_without_null_elements = false;
  googlesql_collation::Options collation = {};

  std::strong_ordering operator<=>(const Options&) const = default;
};

}  // namespace bigquery_emulator_duckdb::functions_duckdb
