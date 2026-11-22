#include "src/googlesql_functions/vector_search_options.h"

#include <string_view>

#include "absl/status/status.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "googlesql/public/json_value.h"

namespace googlesql_functions {
absl::Status CheckVectorSearchOptions(std::string_view text) {
  const auto malformed = [&] {
    return absl::InvalidArgumentError(absl::StrCat("Malformated options: ", text));
  };
  const auto parsed = googlesql::JSONValue::ParseJSONString(text);
  if (!parsed.ok() || !parsed->GetConstRef().IsObject()) {
    return malformed();
  }
  // Whether use_brute_force is set: true, or any string but "false" and the malformed "", and
  // whether fraction_lists_to_search is given other than as the null that BigQuery ignores.
  bool brute_force = false;
  bool fraction = false;
  for (const auto& [name, value] : parsed->GetConstRef().GetMembers()) {
    if (name == "use_brute_force") {
      // BigQuery also takes the boolean as any string.
      if (value.IsBoolean()) {
        brute_force = value.GetBoolean();
      } else if (value.IsString()) {
        if (value.GetString().empty()) {
          return malformed();
        }
        brute_force = value.GetString() != "false";
      } else {
        return malformed();
      }
    } else if (name == "fraction_lists_to_search" && !value.IsNull()) {
      // BigQuery also takes the number as a string, even NaN, which is in no range it rejects.
      double number = 0;
      if (value.IsString()) {
        if (!absl::SimpleAtod(value.GetString(), &number)) {
          return malformed();
        }
      } else if (!value.IsNumber()) {
        return malformed();
      } else {
        number = value.IsInt64()    ? static_cast<double>(value.GetInt64())
                 : value.IsUInt64() ? static_cast<double>(value.GetUInt64())
                                    : value.GetDouble();
      }
      if (number <= 0 || number > 1) {
        return absl::InvalidArgumentError(
            absl::StrCat("fraction_lists_to_search in options of VECTOR_SEARCH should be in the "
                         "range of (0, 1]. Got ",
                         number, "."));
      }
      fraction = true;
    } else if (name != "fraction_lists_to_search") {
      return malformed();
    }
  }
  if (fraction && brute_force) {
    return absl::InvalidArgumentError(
        "fraction_lists_to_search should not be specified when use_brute_force is set.");
  }
  return absl::OkStatus();
}

}  // namespace googlesql_functions
