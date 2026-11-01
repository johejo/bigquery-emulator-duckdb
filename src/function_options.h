#pragma once

#include "src/googlesql_functions_duckdb/options.h"
#include "src/timestamp_precision.h"

namespace bigquery_emulator_duckdb {

// The emulator's choice of every BigQuery difference, kept outside the libraries and adapters.
constexpr functions_duckdb::Options BigQueryFunctionOptions() {
  return {
      .timestamp_precision = TimestampPrecision::kMicroseconds,
      .chr_zero_is_empty = true,
      .format_specifiers = true,
      .unsigned_nan = true,
      .signed_zero_strings = true,
      .unpadded_iso_year = true,
      .extended_array_limits = true,
      .rounding_early_positive_zero = true,
      .json_strict_numbers = true,
      .json_preserve_negative_zero = true,
      .json_keys_rejects_safe = true,
      .collate_und_ci_only = true,
      .intersect_except_distinct_only = true,
      .scalar_window_partition_keys_only = true,
      .distinct_set_operations_without_structs = true,
      .arrays_without_null_elements = true,
      .collation = {.like_unescaped_underscore = true},
  };
}

}  // namespace bigquery_emulator_duckdb
