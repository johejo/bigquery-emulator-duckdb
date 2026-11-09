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
      .double_average = true,
      .double_sum = true,
      .json_strict_numbers = true,
      .json_preserve_negative_zero = true,
      .json_value_error_context = true,
      .collate_und_ci_only = true,
      .query =
          {
              .json_keys_rejects_safe = true,
              .array_first_n_bigquery = true,
              .intersect_except_distinct_only = true,
              .scalar_window_partition_keys_only = true,
              .distinct_set_operations_without_structs = true,
              .distinct_set_operations_without_arrays = true,
              .distinct_aggregates_without_structs = true,
              .distinct_aggregates_without_arrays = true,
              .interval_multiply_int64_only = true,
              .distinct_comparisons_without_structs = true,
              .approx_top_scalar_values_only = true,
              .selections_without_intervals = true,
              .arrays_without_null_elements = true,
              .restrict_struct_results = true,
              .cast_without_type_parameters = true,
              .restrict_pivot_aggregates = true,
          },
      .collation = {.like_unescaped_underscore = true},
  };
}

}  // namespace bigquery_emulator_duckdb
