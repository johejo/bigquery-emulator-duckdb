#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "src/googlesql_collation/options.h"

// Collations over GoogleSQL's collator, which compares strings with ICU.
// third_party/bigquery_docs/reference/standard-sql/collation-concepts.md
namespace googlesql_collation {

// Fails for a collation specification GoogleSQL's collator does not take.
// BigQuery's narrower set of COLLATE specifications is checked by the translator.
absl::Status CheckSpecification(std::string_view specification);

// The sort key of `value` under `specification`: keys compare bytewise as the collation compares
// their values, and are equal exactly when the values are, so a key can stand in for its value
// in comparisons, sorting and grouping. The key under binary collation is the value itself.
absl::StatusOr<std::string> SortKey(std::string_view value, std::string_view specification);

// Search and replacement use GoogleSQL's ICU implementations, not sort keys: their positions
// and returned substrings refer to the original text.
absl::StatusOr<bool> Like(std::string_view specification, std::string_view value,
                          std::string_view pattern, const Options& options);
absl::StatusOr<bool> StartsWith(std::string_view specification, std::string_view value,
                                std::string_view substring);
absl::StatusOr<bool> EndsWith(std::string_view specification, std::string_view value,
                              std::string_view substring);
absl::StatusOr<int64_t> Instr(std::string_view specification, std::string_view value,
                              std::string_view substring, int64_t position, int64_t occurrence);
absl::StatusOr<std::string> Replace(std::string_view specification, std::string_view value,
                                    std::string_view old_substring, std::string_view new_substring);
absl::StatusOr<std::vector<std::string>> Split(std::string_view specification,
                                               std::string_view value, std::string_view delimiter);

}  // namespace googlesql_collation
