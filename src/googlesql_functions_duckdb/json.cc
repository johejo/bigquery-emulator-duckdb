#include "googlesql/public/functions/json.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "duckdb.h"
#include "googlesql/public/functions/json_format.h"
#include "googlesql/public/functions/json_internal.h"
#include "googlesql/public/functions/to_json.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "nlohmann/json.hpp"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_functions_duckdb/any_arguments.h"
#include "src/googlesql_functions_duckdb/json.h"
#include "src/language_options.h"

namespace bigquery_emulator_duckdb::functions_duckdb {
using namespace duckdb_functions;
namespace {

// How GoogleSQL reads JSON values under the analyzer's language options, which round wide numbers
// unless strict number parsing is on.
googlesql::JSONParsingOptions::WideNumberMode JsonNumberMode() {
  return GoogleSqlLanguageOptions().LanguageFeatureEnabled(
             googlesql::FEATURE_JSON_STRICT_NUMBER_PARSING)
             ? googlesql::JSONParsingOptions::WideNumberMode::kExact
             : googlesql::JSONParsingOptions::WideNumberMode::kRound;
}

// JSON goes in and out of the JSON functions as its text.
absl::StatusOr<googlesql::JSONValue> ParseJson(const std::string& text) {
  return googlesql::JSONValue::ParseJSONString(text, {.wide_number_mode = JsonNumberMode()});
}

absl::StatusOr<googlesql::JSONValue> ParseJson(const std::string& text,
                                               googlesql::JSONParsingOptions::WideNumberMode mode) {
  return googlesql::JSONValue::ParseJSONString(text, {.wide_number_mode = mode});
}

// A wide_number_mode argument, 'exact' or 'round'.
absl::StatusOr<googlesql::functions::WideNumberMode> WideNumberMode(const std::string& mode,
                                                                    std::string_view message) {
  if (mode == "exact") {
    return googlesql::functions::WideNumberMode::kExact;
  }
  if (mode == "round") {
    return googlesql::functions::WideNumberMode::kRound;
  }
  return absl::OutOfRangeError(std::string(message) + mode);
}

// A JSON path argument, which is checked even when the JSON is NULL; null for a NULL path.
absl::StatusOr<std::unique_ptr<googlesql::functions::json_internal::StrictJSONPathIterator>>
JsonPathArgument(const Arguments& arguments, idx_t column, const std::string& function) {
  if (arguments.IsNull(column)) {
    return nullptr;
  }
  auto path =
      googlesql::functions::json_internal::StrictJSONPathIterator::Create(arguments.String(column));
  if (!path.ok()) {
    return absl::OutOfRangeError("Invalid input to " + function + ": " +
                                 std::string(path.status().message()));
  }
  return path;
}

// PARSE_JSON(string, wide_number_mode).
void ParseJsonFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const std::string mode = arguments.String(1);
    if (mode != "exact" && mode != "round") {
      return absl::OutOfRangeError("Invalid `wide_number_mode` specified for PARSE_JSON: " + mode);
    }
    const auto json =
        ParseJson(arguments.String(0), mode == "exact"
                                           ? googlesql::JSONParsingOptions::WideNumberMode::kExact
                                           : googlesql::JSONParsingOptions::WideNumberMode::kRound);
    if (!json.ok()) {
      return absl::OutOfRangeError("Invalid input to PARSE_JSON: " +
                                   std::string(json.status().message()));
    }
    return json->GetConstRef().ToString();
  });
}

// TO_JSON_STRING(value, pretty_print), where a NULL value is JSON null.
void ToJsonString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const AnyArguments values(input, {0});
  EachRow(
      info, input, output,
      [&values](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        if (arguments.IsNull(1)) {
          return std::nullopt;
        }
        const auto value = values.Get(0, arguments.Row());
        if (!value.ok()) {
          return value.status();
        }
        googlesql::functions::JsonPrettyPrinter printer(arguments.Bool(1),
                                                        googlesql::PRODUCT_EXTERNAL);
        std::string out;
        const absl::Status status = googlesql::functions::JsonFromValue(
            *value, &printer, &out,
            {.wide_number_mode = JsonNumberMode(), .canonicalize_zero = true});
        return ToStatusOr(status.ok(), std::optional<std::string>(out), status);
      },
      /*nulls=*/false);
}

// TO_JSON(value, stringify_wide_numbers), where a NULL value is JSON null.
void ToJson(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const AnyArguments values(input, {0});
  EachRow(
      info, input, output,
      [&values](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        if (arguments.IsNull(1)) {
          return std::nullopt;
        }
        const auto value = values.Get(0, arguments.Row());
        if (!value.ok()) {
          return value.status();
        }
        const auto json =
            googlesql::functions::ToJson(*value, arguments.Bool(1), GoogleSqlLanguageOptions(),
                                         /*canonicalize_zero=*/true);
        if (!json.ok()) {
          return json.status();
        }
        return json->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// JSON_ARRAY(values...).
void JsonArray(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const AnyArguments values(input);
  const idx_t columns = duckdb_data_chunk_get_column_count(input);
  EachRow(
      info, input, output,
      [&](const Arguments& arguments) -> absl::StatusOr<std::string> {
        std::vector<googlesql::Value> elements;
        for (idx_t column = 0; column < columns; ++column) {
          auto value = values.Get(column, arguments.Row());
          if (!value.ok()) {
            return value.status();
          }
          elements.push_back(*std::move(value));
        }
        const auto json = googlesql::functions::JsonArray(elements, GoogleSqlLanguageOptions(),
                                                          /*canonicalize_zero=*/true);
        if (!json.ok()) {
          return absl::OutOfRangeError("Invalid input to JSON_ARRAY: " +
                                       std::string(json.status().message()));
        }
        return json->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// JSON_OBJECT(key, value, ...) or JSON_OBJECT(keys, values).
void JsonObject(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const AnyArguments values(input);
  const idx_t columns = duckdb_data_chunk_get_column_count(input);
  const bool arrays = columns == 2 && values.Type(0) != nullptr && values.Type(0)->IsArray();
  EachRow(
      info, input, output,
      [&](const Arguments& arguments) -> absl::StatusOr<std::string> {
        const auto invalid = [](std::string_view message) {
          return absl::OutOfRangeError("Invalid input to JSON_OBJECT: " + std::string(message));
        };
        std::vector<googlesql::Value> arguments_list;
        for (idx_t column = 0; column < columns; ++column) {
          auto value = values.Get(column, arguments.Row());
          if (!value.ok()) {
            return value.status();
          }
          arguments_list.push_back(*std::move(value));
        }
        std::vector<googlesql::Value> keys;
        std::vector<const googlesql::Value*> pointers;
        if (arrays) {
          if (arguments_list.at(0).is_null()) {
            return invalid("The keys array cannot be NULL");
          }
          if (arguments_list.at(1).is_null()) {
            return invalid("The values array cannot be NULL");
          }
          if (arguments_list.at(0).num_elements() != arguments_list.at(1).num_elements()) {
            return invalid("The number of keys and values must match");
          }
          for (int i = 0; i < arguments_list.at(0).num_elements(); ++i) {
            keys.push_back(arguments_list.at(0).element(i));
            pointers.push_back(&arguments_list.at(1).element(i));
          }
        } else {
          for (idx_t i = 0; i + 1 < columns; i += 2) {
            keys.push_back(arguments_list.at(i));
            pointers.push_back(&arguments_list.at(i + 1));
          }
        }
        std::vector<absl::string_view> key_views;
        for (const googlesql::Value& key : keys) {
          if (key.is_null()) {
            return invalid("A key cannot be NULL");
          }
          key_views.push_back(key.string_value());
        }
        googlesql::functions::JsonObjectBuilder builder(GoogleSqlLanguageOptions(),
                                                        /*canonicalize_zero=*/true);
        const auto object =
            googlesql::functions::JsonObject(key_views, absl::MakeSpan(pointers), builder);
        if (!object.ok()) {
          return invalid(object.status().message());
        }
        return object->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// LAX_BOOL, LAX_INT64, LAX_FLOAT64 and LAX_STRING, which are NULL for JSON of another type.
template <typename T, absl::StatusOr<std::optional<T>> (*kConvert)(googlesql::JSONValueConstRef)>
void LaxConvert(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::optional<T>> {
    const auto document = ParseJson(arguments.String(0));
    if (!document.ok()) {
      return document.status();
    }
    return kConvert(document->GetConstRef());
  });
}

// BOOL, INT64 and STRING of JSON, which fail for JSON of another type.
template <typename T, absl::StatusOr<T> (*kConvert)(googlesql::JSONValueConstRef)>
void ConvertJson(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<T> {
    const auto document = ParseJson(arguments.String(0));
    if (!document.ok()) {
      return document.status();
    }
    return kConvert(document->GetConstRef());
  });
}

// FLOAT64(json, wide_number_mode), which also parses the JSON in that mode.
void JsonToFloat64(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<double> {
    const auto mode = WideNumberMode(arguments.String(1), "Invalid `wide_number_mode` specified: ");
    if (!mode.ok()) {
      return mode.status();
    }
    const auto document =
        ParseJson(arguments.String(0), *mode == googlesql::functions::WideNumberMode::kExact
                                           ? googlesql::JSONParsingOptions::WideNumberMode::kExact
                                           : googlesql::JSONParsingOptions::WideNumberMode::kRound);
    if (!document.ok()) {
      return document.status();
    }
    return googlesql::functions::ConvertJsonToDouble(document->GetConstRef(), *mode,
                                                     googlesql::PRODUCT_EXTERNAL);
  });
}

// JSON_TYPE(json).
void JsonType(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto document = ParseJson(arguments.String(0));
    if (!document.ok()) {
      return document.status();
    }
    return googlesql::functions::GetJsonType(document->GetConstRef());
  });
}

// What a JSON extraction function returns: the JSON or the scalar at the path, or the array
// there of JSON or of scalars.
enum class Extraction : std::uint8_t { kQuery, kValue, kQueryArray, kValueArray };

// JSON_QUERY, JSON_VALUE, JSON_QUERY_ARRAY and JSON_VALUE_ARRAY, and their legacy JSON_EXTRACT
// forms, taking (json, path, standard), where `standard` is false for the legacy JSONPath. The
// JSON is a STRING unless kJson; a malformed STRING is NULL.
template <Extraction kExtraction, bool kJson>
void JsonExtract(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  using Result =
      std::conditional_t<kExtraction == Extraction::kQueryArray, std::vector<std::string>,
                         std::conditional_t<kExtraction == Extraction::kValueArray,
                                            std::vector<std::optional<std::string>>, std::string>>;
  // Keep one prepared path per chunk, including its legacy/standard interpretation.
  // Borrowed path text remains valid until the scalar callback returns.
  std::string_view previous_path;
  bool previous_standard = false;
  std::unique_ptr<googlesql::functions::JsonPathEvaluator> evaluator;
  std::unique_ptr<googlesql::functions::json_internal::StrictJSONPathIterator> lax_iterator;
  EachRow(info, input, output,
          [&](const Arguments& arguments) -> absl::StatusOr<std::optional<Result>> {
            const std::string_view path = arguments.StringView(1);
            const bool standard = arguments.Bool(2);
            // Assigned in the kJson branch, which const-correctness does not see.
            // NOLINTNEXTLINE(misc-const-correctness)
            std::optional<googlesql::JSONValue> document;
            if constexpr (kJson) {
              auto parsed = ParseJson(arguments.String(0));
              if (!parsed.ok()) {
                return parsed.status();
              }
              document = *std::move(parsed);
            }
            if ((!evaluator && !lax_iterator) || path != previous_path ||
                standard != previous_standard) {
              evaluator.reset();
              lax_iterator.reset();
              if constexpr (kJson && kExtraction == Extraction::kQuery) {
                // JSON_QUERY of JSON takes a lax JSONPath, such as "lax $.a".
                const auto lax = googlesql::functions::json_internal::IsValidAndLaxJSONPath(path);
                if (!lax.ok()) {
                  return lax.status();
                }
                if (standard && *lax) {
                  auto iterator =
                      googlesql::functions::json_internal::StrictJSONPathIterator::Create(
                          path, /*enable_lax_mode=*/true);
                  if (!iterator.ok()) {
                    return iterator.status();
                  }
                  lax_iterator = *std::move(iterator);
                }
              }
              if (!lax_iterator) {
                auto prepared = googlesql::functions::JsonPathEvaluator::Create(
                    path, standard, /*enable_special_character_escaping_in_values=*/true,
                    /*enable_special_character_escaping_in_keys=*/true);
                if (!prepared.ok()) {
                  return prepared.status();
                }
                evaluator = *std::move(prepared);
              }
              previous_path = path;
              previous_standard = standard;
            }
            if constexpr (kJson && kExtraction == Extraction::kQuery) {
              if (lax_iterator) {
                const auto result =
                    googlesql::functions::JsonQueryLax(document->GetConstRef(), *lax_iterator);
                if (!result.ok()) {
                  return result.status();
                }
                return result->GetConstRef().ToString();
              }
            }
            if constexpr (kJson) {
              const googlesql::JSONValueConstRef json = document->GetConstRef();
              if constexpr (kExtraction == Extraction::kQuery) {
                const auto value = evaluator->Extract(json);
                return value ? std::optional<std::string>(value->ToString()) : std::nullopt;
              } else if constexpr (kExtraction == Extraction::kValue) {
                return evaluator->ExtractScalar(json);
              } else if constexpr (kExtraction == Extraction::kQueryArray) {
                const auto elements = evaluator->ExtractArray(json);
                if (!elements) {
                  return std::nullopt;
                }
                std::vector<std::string> texts;
                std::ranges::transform(
                    *elements, std::back_inserter(texts),
                    [](googlesql::JSONValueConstRef element) { return element.ToString(); });
                return texts;
              } else {
                return evaluator->ExtractStringArray(json);
              }
            } else {
              const std::string_view json = arguments.StringView(0);
              Result out;
              bool is_null = false;
              absl::Status status;
              if constexpr (kExtraction == Extraction::kQuery) {
                status = evaluator->Extract(json, &out, &is_null);
              } else if constexpr (kExtraction == Extraction::kValue) {
                status = evaluator->ExtractScalar(json, &out, &is_null);
              } else if constexpr (kExtraction == Extraction::kQueryArray) {
                status = evaluator->ExtractArray(json, &out, &is_null);
              } else {
                status = evaluator->ExtractStringArray(json, &out, &is_null);
              }
              if (!status.ok()) {
                return status;
              }
              return is_null ? std::nullopt : std::optional<Result>(std::move(out));
            }
          });
}

// json[key] and json.key, which are NULL unless the JSON is an object with the key.
void JsonField(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
            const auto document = ParseJson(arguments.String(0));
            if (!document.ok()) {
              return document.status();
            }
            const auto member = document->GetConstRef().GetMemberIfExists(arguments.String(1));
            return member ? std::optional<std::string>(member->ToString()) : std::nullopt;
          });
}

// json[index], which is NULL unless the JSON is an array with the index.
void JsonElement(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const auto document = ParseJson(arguments.String(0));
        if (!document.ok()) {
          return document.status();
        }
        const googlesql::JSONValueConstRef json = document->GetConstRef();
        const int64_t index = arguments.Int(1);
        if (!json.IsArray() || index < 0 || std::cmp_greater_equal(index, json.GetArraySize())) {
          return std::nullopt;
        }
        return json.GetArrayElement(index).ToString();
      });
}

// JSON_FLATTEN returns JSON elements as text, cast to JSON[] by the rule.
void JsonFlatten(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::vector<std::string>> {
            const auto document = ParseJson(arguments.String(0));
            if (!document.ok()) {
              return document.status();
            }
            std::vector<std::string> elements;
            std::ranges::transform(
                googlesql::functions::JsonFlatten(document->GetConstRef()),
                std::back_inserter(elements),
                [](googlesql::JSONValueConstRef element) { return element.ToString(); });
            return elements;
          });
}

// JSON_KEYS(json, max_depth, mode), as the JSON array of the keys.
void JsonKeys(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  using googlesql::functions::json_internal::JsonPathOptions;
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        // BigQuery validates mode before depth, even when the JSON input is SQL NULL.
        JsonPathOptions options = JsonPathOptions::kStrict;
        if (!arguments.IsNull(2)) {
          const std::string mode = arguments.String(2);
          if (absl::EqualsIgnoreCase(mode, "lax")) {
            options = JsonPathOptions::kLax;
          } else if (absl::EqualsIgnoreCase(mode, "lax recursive")) {
            options = JsonPathOptions::kLaxRecursive;
          } else if (!absl::EqualsIgnoreCase(mode, "strict")) {
            return absl::OutOfRangeError("Invalid JSON mode specified: " + mode +
                                         "; error in JSON_KEYS expression");
          }
        }
        const int64_t max_depth =
            arguments.IsNull(1) ? std::numeric_limits<int64_t>::max() : arguments.Int(1);
        if (max_depth <= 0) {
          return absl::OutOfRangeError(
              "max_depth must be positive. provided value: " + std::to_string(max_depth) +
              "; error in JSON_KEYS expression");
        }
        if (arguments.IsNull(0) || arguments.IsNull(2)) {
          return std::nullopt;
        }
        const auto document = ParseJson(arguments.String(0));
        if (!document.ok()) {
          return document.status();
        }
        const auto keys = googlesql::functions::JsonKeys(
            document->GetConstRef(), {.path_options = options, .max_depth = max_depth});
        if (!keys.ok()) {
          return keys.status();
        }
        return nlohmann::json(*keys).dump();
      },
      /*nulls=*/false);
}

// JSON_REMOVE(json, path) for one path; a NULL path removes nothing.
void JsonRemove(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const auto path = JsonPathArgument(arguments, 1, "JSON_REMOVE");
        if (!path.ok()) {
          return path.status();
        }
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        auto document = ParseJson(arguments.String(0));
        if (!document.ok()) {
          return document.status();
        }
        if (*path != nullptr) {
          const auto removed = googlesql::functions::JsonRemove(document->GetRef(), **path);
          if (!removed.ok()) {
            return removed.status();
          }
        }
        return document->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// JSON_SET(json, path, value, create_if_missing) for one path, with a value of any type; a NULL
// path or create_if_missing sets nothing.
void JsonSet(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const AnyArguments values(input, {2});
  EachRow(
      info, input, output,
      [&values](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const auto path = JsonPathArgument(arguments, 1, "JSON_SET");
        if (!path.ok()) {
          return path.status();
        }
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        auto document = ParseJson(arguments.String(0));
        if (!document.ok()) {
          return document.status();
        }
        if (*path == nullptr || arguments.IsNull(3)) {
          return document->GetConstRef().ToString();
        }
        const auto value = values.Get(2, arguments.Row());
        if (!value.ok()) {
          return value.status();
        }
        const absl::Status status =
            googlesql::functions::JsonSet(document->GetRef(), **path, *value, arguments.Bool(3),
                                          GoogleSqlLanguageOptions(), /*canonicalize_zero=*/true);
        if (!status.ok()) {
          return absl::OutOfRangeError("Invalid input to JSON_SET: " +
                                       std::string(status.message()));
        }
        return document->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// Validate every path before checking NULL input or the flag, then apply pairs in order.
// A SQL array is expanded only when the flag is true; JSON arrays remain single values.
template <bool kInsert>
void JsonArrayModify(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const idx_t columns = duckdb_data_chunk_get_column_count(input);
  // BigQuery's TO_JSON encoding rejects lossy decimals, where GoogleSQL's default rounds.
  googlesql::LanguageOptions options = GoogleSqlLanguageOptions();
  options.EnableLanguageFeature(googlesql::FEATURE_JSON_STRICT_NUMBER_PARSING);
  std::vector<idx_t> paired;
  for (idx_t i = 2; i + 1 < columns; i += 2) {
    paired.push_back(i);
  }
  const AnyArguments values(input, paired);
  EachRow(
      info, input, output,
      [&values, &options,
       columns](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const std::string function = kInsert ? "JSON_ARRAY_INSERT" : "JSON_ARRAY_APPEND";
        std::vector<std::unique_ptr<googlesql::functions::json_internal::StrictJSONPathIterator>>
            paths;
        for (idx_t i = 1; i + 1 < columns; i += 2) {
          auto path = JsonPathArgument(arguments, i, function);
          if (!path.ok()) {
            return path.status();
          }
          paths.push_back(*std::move(path));
        }
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        auto document = ParseJson(arguments.String(0));
        if (!document.ok()) {
          return document.status();
        }
        if (arguments.IsNull(columns - 1)) {
          return document->GetConstRef().ToString();
        }
        for (idx_t i = 0; i < paths.size(); ++i) {
          if (paths.at(i) == nullptr) {
            continue;
          }
          const auto value = values.Get((2 * i) + 2, arguments.Row());
          if (!value.ok()) {
            return value.status();
          }
          const auto modify = kInsert ? googlesql::functions::JsonInsertArrayElement
                                      : googlesql::functions::JsonAppendArrayElement;
          // BigQuery keeps the sign of a negative zero it inserts.
          const absl::Status status =
              modify(document->GetRef(), *paths.at(i), *value, options,
                     /*canonicalize_zero=*/false, arguments.Bool(columns - 1));
          if (!status.ok()) {
            return absl::OutOfRangeError("Invalid input to " + function + ": " +
                                         std::string(status.message()));
          }
        }
        return document->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// JSON_STRIP_NULLS(json, path, include_arrays, remove_empty); a NULL argument past the JSON
// strips nothing.
void JsonStripNulls(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const auto path = JsonPathArgument(arguments, 1, "JSON_STRIP_NULLS");
        if (!path.ok()) {
          return path.status();
        }
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        auto document = ParseJson(arguments.String(0));
        if (!document.ok()) {
          return document.status();
        }
        if (*path != nullptr && !arguments.IsNull(2) && !arguments.IsNull(3)) {
          absl::Status status = googlesql::functions::JsonStripNulls(
              document->GetRef(), **path, arguments.Bool(2), arguments.Bool(3));
          if (!status.ok()) {
            return status;
          }
        }
        return document->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

}  // namespace

void RegisterJsonFunctions(duckdb_connection connection) {
  Register(connection, "bq_lax_bool", {kVarchar}, kBoolean,
           LaxConvert<bool, googlesql::functions::LaxConvertJsonToBool>);
  Register(connection, "bq_lax_int64", {kVarchar}, kBigint,
           LaxConvert<int64_t, googlesql::functions::LaxConvertJsonToInt64>);
  Register(connection, "bq_lax_float64", {kVarchar}, kDouble,
           LaxConvert<double, googlesql::functions::LaxConvertJsonToFloat64>);
  Register(connection, "bq_lax_string", {kVarchar}, kVarchar,
           LaxConvert<std::string, googlesql::functions::LaxConvertJsonToString>);
  Register(connection, "bq_json_bool", {kVarchar}, kBoolean,
           ConvertJson<bool, googlesql::functions::ConvertJsonToBool>);
  Register(connection, "bq_json_int64", {kVarchar}, kBigint,
           ConvertJson<int64_t, googlesql::functions::ConvertJsonToInt64>);
  Register(connection, "bq_json_string", {kVarchar}, kVarchar,
           ConvertJson<std::string, googlesql::functions::ConvertJsonToString>);
  Register(connection, "bq_json_float64", {kVarchar, kVarchar}, kDouble, JsonToFloat64);
  Register(connection, "bq_json_type", {kVarchar}, kVarchar, JsonType);
  Register(connection, "bq_parse_json", {kVarchar, kVarchar}, kVarchar, ParseJsonFunction);
  Register(connection, "bq_json_field", {kVarchar, kVarchar}, kVarchar, JsonField);
  Register(connection, "bq_json_element", {kVarchar, kBigint}, kVarchar, JsonElement);
  Register(connection, "bq_to_json_string", {kAny, kBoolean}, kVarchar, ToJsonString,
           /*nulls=*/false);
  Register(connection, "bq_to_json", {kAny, kBoolean}, kVarchar, ToJson, /*nulls=*/false);
  Register(connection, "bq_json_array", {}, kVarchar, JsonArray, /*nulls=*/false, kAny);
  Register(connection, "bq_json_object", {}, kVarchar, JsonObject, /*nulls=*/false, kAny);
  {
    LogicalType const varchar(duckdb_create_logical_type(kVarchar));
    LogicalType const list(duckdb_create_list_type(varchar.get()));
    Register(connection, "bq_json_flatten", {kVarchar}, list.get(), JsonFlatten);
    for (const auto& [name, function, array] :
         std::initializer_list<std::tuple<const char*, duckdb_scalar_function_t, bool>>{
             {"bq_json_query", JsonExtract<Extraction::kQuery, false>, false},
             {"bq_json_query_json", JsonExtract<Extraction::kQuery, true>, false},
             {"bq_json_value", JsonExtract<Extraction::kValue, false>, false},
             {"bq_json_value_json", JsonExtract<Extraction::kValue, true>, false},
             {"bq_json_query_array", JsonExtract<Extraction::kQueryArray, false>, true},
             {"bq_json_query_array_json", JsonExtract<Extraction::kQueryArray, true>, true},
             {"bq_json_value_array", JsonExtract<Extraction::kValueArray, false>, true},
             {"bq_json_value_array_json", JsonExtract<Extraction::kValueArray, true>, true},
         }) {
      Register(connection, name, {kVarchar, kVarchar, kBoolean}, array ? list.get() : varchar.get(),
               function);
    }
  }
  Register(connection, "bq_json_keys", {kVarchar, kBigint, kVarchar}, kVarchar, JsonKeys,
           /*nulls=*/false);
  Register(connection, "bq_json_remove", {kVarchar, kVarchar}, kVarchar, JsonRemove,
           /*nulls=*/false);
  Register(connection, "bq_json_set", {kVarchar, kVarchar, kAny, kBoolean}, kVarchar, JsonSet,
           /*nulls=*/false);
  Register(connection, "bq_json_array_append", {kVarchar}, kVarchar, JsonArrayModify<false>,
           /*nulls=*/false, kAny);
  Register(connection, "bq_json_array_insert", {kVarchar}, kVarchar, JsonArrayModify<true>,
           /*nulls=*/false, kAny);
  Register(connection, "bq_json_strip_nulls", {kVarchar, kVarchar, kBoolean, kBoolean}, kVarchar,
           JsonStripNulls, /*nulls=*/false);
}

}  // namespace bigquery_emulator_duckdb::functions_duckdb
