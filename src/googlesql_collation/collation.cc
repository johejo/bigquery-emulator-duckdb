#include "src/googlesql_collation/collation.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "absl/base/const_init.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/synchronization/mutex.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/collator.h"

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

}  // namespace

absl::Status CheckSpecification(std::string_view specification, bool bigquery) {
  if (specification.empty()) {
    return absl::OkStatus();
  }
  if (bigquery && specification != "und:ci") {
    return absl::InvalidArgumentError("Collation '" + std::string(specification) +
                                      "' is not supported; use 'und:ci' or ''");
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

}  // namespace googlesql_collation
