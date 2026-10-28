#pragma once

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

// Collations over GoogleSQL's collator, which compares strings with ICU.
// third_party/bigquery_docs/reference/standard-sql/collation-concepts.md
namespace googlesql_collation {

// Fails for a collation specification GoogleSQL's collator does not take, or with `bigquery`, for
// one BigQuery does not take: BigQuery takes only `und:ci` and the empty specification, binary
// collation, where GoogleSQL also takes other locales, `und:cs` and `binary`.
absl::Status CheckSpecification(std::string_view specification, bool bigquery);

// The sort key of `value` under `specification`: keys compare bytewise as the collation compares
// their values, and are equal exactly when the values are, so a key can stand in for its value
// in comparisons, sorting and grouping. The key under binary collation is the value itself.
absl::StatusOr<std::string> SortKey(std::string_view value, std::string_view specification,
                                    bool bigquery);

}  // namespace googlesql_collation
