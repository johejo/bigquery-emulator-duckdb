#include "src/googlesql_functions/contains_substr.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/cast.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/value.h"
#include "src/googlesql_functions/string.h"
#include "src/language_options.h"

namespace googlesql_functions {
namespace {

// The needle is normalized once, before visiting any fields or elements.
class SubstringSearch {
 public:
  SubstringSearch(std::string needle, std::string_view scope)
      : needle_(std::move(needle)), keys_(scope != "JSON_VALUES"), values_(scope != "JSON_KEYS") {}

  absl::StatusOr<bool> Text(std::string_view text) const {
    std::string normalized;
    absl::Status error;
    if (!NormalizeAndCasefold(text, "NFKC", &normalized, &error)) {
      return error;
    }
    return normalized.find(needle_) != std::string::npos;
  }

  absl::StatusOr<bool> Json(googlesql::JSONValueConstRef json) const {
    if (json.IsObject()) {
      for (const auto& [key, value] : json.GetMembers()) {
        if (keys_) {
          GOOGLESQL_ASSIGN_OR_RETURN(const bool found, Text(key));
          if (found) {
            return true;
          }
        }
        GOOGLESQL_ASSIGN_OR_RETURN(const bool found, Json(value));
        if (found) {
          return true;
        }
      }
      return false;
    }
    if (json.IsArray()) {
      // Each recursive call can return an error, which an any_of predicate cannot propagate.
      // cppcheck-suppress useStlAlgorithm
      for (const auto element : json.GetArrayElements()) {
        GOOGLESQL_ASSIGN_OR_RETURN(const bool found, Json(element));
        if (found) {
          return true;
        }
      }
      return false;
    }
    if (!values_) {
      return false;
    }
    // BigQuery searches JSON null as the text "null", unlike a SQL NULL leaf.
    return Text(json.IsString() ? json.GetString() : json.ToString());
  }

  absl::StatusOr<std::optional<bool>> Value(const googlesql::Value& value) const {
    if (value.is_null()) {
      return std::nullopt;
    }
    // BigQuery returns NULL for these leaves rather than searching their STRING casts.
    // The DuckDB adapter also holds GEOGRAPHY as BYTES here; neither payload is searched.
    if (value.type()->IsBytes() || value.type()->IsInterval() || value.type()->IsRange()) {
      return std::nullopt;
    }
    if (value.type()->IsArray() || value.type()->IsStruct()) {
      bool has_null = false;
      GOOGLESQL_ASSIGN_OR_RETURN(const auto children, value.type()->IsArray()
                                                          ? value.elements_view()
                                                          : value.fields_view());
      for (const auto& child : children) {
        GOOGLESQL_ASSIGN_OR_RETURN(const auto found, Value(child));
        if (found.value_or(false)) {
          return true;
        }
        has_null |= !found.has_value();
      }
      return has_null ? std::optional<bool>() : std::optional<bool>(false);
    }
    if (value.type()->IsJson()) {
      GOOGLESQL_ASSIGN_OR_RETURN(const bool found, Json(value.json_value()));
      return found;
    }
    GOOGLESQL_ASSIGN_OR_RETURN(
        const auto text, googlesql::CastValue(value, absl::UTCTimeZone(),
                                              bigquery_emulator_duckdb::GoogleSqlLanguageOptions(),
                                              googlesql::types::StringType()));
    GOOGLESQL_ASSIGN_OR_RETURN(const bool found, Text(text.string_value()));
    return found;
  }

 private:
  std::string needle_;
  bool keys_;
  bool values_;
};

}  // namespace

absl::StatusOr<std::optional<bool>> ContainsSubstr(const googlesql::Value& value,
                                                   std::string_view search,
                                                   std::optional<std::string_view> json_scope) {
  // NULL json_scope selects JSON_VALUES. Validate before searching even a SQL NULL input.
  const auto scope = json_scope.value_or("JSON_VALUES");
  if (scope != "JSON_VALUES" && scope != "JSON_KEYS" && scope != "JSON_KEYS_AND_VALUES") {
    return absl::OutOfRangeError("Invalid json_scope value '" + std::string(scope) + "'.");
  }
  std::string needle;
  absl::Status error;
  if (!NormalizeAndCasefold(search, "NFKC", &needle, &error)) {
    return error;
  }
  return SubstringSearch(std::move(needle), scope).Value(value);
}

}  // namespace googlesql_functions
