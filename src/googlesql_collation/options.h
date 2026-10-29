#pragma once

#include <compare>

namespace googlesql_collation {

// Which of BigQuery's differences from GoogleSQL's collator the functions adapt. Defaults keep
// GoogleSQL's behavior.
struct Options {
  // Specifications are und:ci or empty, binary collation, where GoogleSQL also takes other
  // locales, `und:cs` and `binary`.
  bool und_ci_only = false;
  // LIKE accepts an unescaped underscore in its pattern. Observed BigQuery accepts it under
  // und:ci, unlike the error example in
  // third_party/bigquery_docs/reference/standard-sql/operators.md; GoogleSQL's compliance data
  // still uses the implementation that rejects it.
  bool like_unescaped_underscore = false;

  std::strong_ordering operator<=>(const Options&) const = default;
};

}  // namespace googlesql_collation
