#include "src/googlesql_collation/collation.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/base/const_init.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/synchronization/mutex.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/collator.h"
#include "googlesql/public/functions/string_with_collation.h"

namespace googlesql_collation {
namespace {

using Collator = absl::StatusOr<std::unique_ptr<const googlesql::GoogleSqlCollator>>;

// The collator of a non-empty specification, built once. Comparisons and sort keys do not modify
// a collator, so concurrent queries share it.
absl::StatusOr<const googlesql::GoogleSqlCollator*> FindCollator(std::string_view specification) {
  static absl::Mutex mutex(absl::kConstInit);
  static auto* const collators = new std::map<std::string, Collator, std::less<>>();
  const Collator* collator = nullptr;
  {
    const absl::ReaderMutexLock lock(mutex);
    if (const auto it = collators->find(specification); it != collators->end()) {
      collator = &it->second;
    }
  }
  if (collator == nullptr) {
    const absl::MutexLock lock(mutex);
    collator = &collators
                    ->try_emplace(std::string(specification),
                                  googlesql::MakeSqlCollator(
                                      specification, googlesql::CollatorLegacyUnicodeMode::kError))
                    .first->second;
  }
  if (!collator->ok()) {
    return collator->status();
  }
  return collator->value().get();
}

template <typename Result, typename Function, typename... Args>
absl::StatusOr<Result> Evaluate(std::string_view specification, bool bigquery, Function function,
                                const Args&... args) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSpecification(specification, bigquery));
  GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::GoogleSqlCollator* collator,
                             FindCollator(specification));
  Result result;
  absl::Status status;
  if (!function(*collator, args..., &result, &status)) {
    return status;
  }
  return result;
}

}  // namespace

absl::Status CheckSpecification(std::string_view specification, bool bigquery) {
  if (specification.empty()) {
    return absl::OkStatus();
  }
  if (bigquery && specification != "und:ci") {
    return absl::InvalidArgumentError("Collation '" + std::string(specification) +
                                      "' in collate function is not supported.");
  }
  return FindCollator(specification).status();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a value before its collation, as in SQL.
absl::StatusOr<std::string> SortKey(std::string_view value, std::string_view specification,
                                    bool bigquery) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSpecification(specification, bigquery));
  if (specification.empty()) {
    return std::string(value);
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::GoogleSqlCollator* collator,
                             FindCollator(specification));
  if (collator->IsBinaryComparison()) {
    return std::string(value);
  }
  absl::Cord key;
  GOOGLESQL_RETURN_IF_ERROR(collator->GetSortKeyUtf8(value, &key));
  return std::string(key);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): collation first, then SQL operands.
absl::StatusOr<bool> Like(std::string_view specification, std::string_view value,
                          std::string_view pattern, bool bigquery) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSpecification(specification, bigquery));
  GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::GoogleSqlCollator* collator,
                             FindCollator(specification));
  // Observed BigQuery accepts unescaped underscores under und:ci, unlike the error
  // example in third_party/bigquery_docs/reference/standard-sql/operators.md.
  // GoogleSQL's compliance data still uses the implementation that rejects them.
  return bigquery
             ? googlesql::functions::LikeUtf8WithCollationAllowUnderscore(value, pattern, *collator)
             : googlesql::functions::LikeUtf8WithCollation(value, pattern, *collator);
}

absl::StatusOr<bool> StartsWith(std::string_view specification, std::string_view value,
                                std::string_view substring, bool bigquery) {
  return Evaluate<bool>(specification, bigquery, googlesql::functions::StartsWithUtf8WithCollation,
                        value, substring);
}

absl::StatusOr<bool> EndsWith(std::string_view specification, std::string_view value,
                              std::string_view substring, bool bigquery) {
  return Evaluate<bool>(specification, bigquery, googlesql::functions::EndsWithUtf8WithCollation,
                        value, substring);
}

absl::StatusOr<int64_t> Instr(std::string_view specification, std::string_view value,
                              std::string_view substring, int64_t position, int64_t occurrence,
                              bool bigquery) {
  return Evaluate<int64_t>(specification, bigquery,
                           googlesql::functions::StrPosOccurrenceUtf8WithCollation, value,
                           substring, position, occurrence);
}

absl::StatusOr<std::string> Replace(std::string_view specification, std::string_view value,
                                    std::string_view old_substring, std::string_view new_substring,
                                    bool bigquery) {
  return Evaluate<std::string>(specification, bigquery,
                               googlesql::functions::ReplaceUtf8WithCollation, value, old_substring,
                               new_substring);
}

absl::StatusOr<std::vector<std::string>> Split(std::string_view specification,
                                               std::string_view value, std::string_view delimiter,
                                               bool bigquery) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      auto parts,
      Evaluate<std::vector<absl::string_view>>(
          specification, bigquery, googlesql::functions::SplitUtf8WithCollation, value, delimiter));
  return std::vector<std::string>(parts.begin(), parts.end());
}

}  // namespace googlesql_collation
