#pragma once

#include <compare>

namespace bigquery_emulator_duckdb::function_catalog {

// Which forms the translator rejects that GoogleSQL accepts but BigQuery does not. Defaults keep
// GoogleSQL's forms.
struct QueryRestrictions {
  // JSON_KEYS takes no SAFE. prefix.
  bool json_keys_rejects_safe = false;
  // INTERSECT and EXCEPT take only DISTINCT.
  bool intersect_except_distinct_only = false;
  // Window PARTITION BY takes no STRUCT or ARRAY keys.
  bool scalar_window_partition_keys_only = false;
  // DISTINCT set operations take no STRUCT columns.
  bool distinct_set_operations_without_structs = false;
  // DISTINCT set operations take no ARRAY columns.
  bool distinct_set_operations_without_arrays = false;
  // Aggregate functions with DISTINCT take no STRUCT arguments.
  bool distinct_aggregates_without_structs = false;
  // Aggregate functions with DISTINCT take no ARRAY arguments.
  bool distinct_aggregates_without_arrays = false;
  // INTERVAL multiplies only by INT64, not FLOAT64.
  bool interval_multiply_int64_only = false;
  // IS DISTINCT FROM and IS NOT DISTINCT FROM take no STRUCT arguments.
  bool distinct_comparisons_without_structs = false;
  // APPROX_TOP_COUNT and APPROX_TOP_SUM take no STRUCT or ARRAY values.
  bool approx_top_scalar_values_only = false;
  // Query results and written values hold no ARRAY with a NULL element.
  bool arrays_without_null_elements = false;
  // Query results take no empty STRUCTs or anonymous fields of STRUCT value tables.
  bool restrict_struct_results = false;

  std::strong_ordering operator<=>(const QueryRestrictions&) const = default;
};

}  // namespace bigquery_emulator_duckdb::function_catalog
