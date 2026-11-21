#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "absl/status/statusor.h"
#include "googlesql/public/value.h"
#include "src/googlesql_text_analysis/text_analysis.h"

namespace googlesql_text_analysis {

struct SearchQuery;

// SEARCH's search_query for `analyzer`, as the search query rules describe it:
// third_party/bigquery_docs/reference/standard-sql/search_functions.md. Forms the rules leave
// open fail as unsupported.
absl::StatusOr<std::shared_ptr<const SearchQuery>> ParseSearchQuery(
    std::string_view query, std::shared_ptr<const Analyzer> analyzer);

// Whether one STRING field of data_to_search satisfies the query.
absl::StatusOr<bool> SearchField(const SearchQuery& query, std::string_view text);

enum class JsonScope : std::uint8_t { kValues, kKeys, kKeysAndValues };
absl::StatusOr<JsonScope> ParseJsonScope(std::string_view scope);

// SEARCH over data_to_search: TRUE when one of its STRING fields, also within ARRAYs, STRUCTs and
// JSON, satisfies the query; otherwise NULL when one of them is NULL; otherwise FALSE.
absl::StatusOr<std::optional<bool>> Search(const googlesql::Value& data, const SearchQuery& query,
                                           JsonScope scope);

}  // namespace googlesql_text_analysis
