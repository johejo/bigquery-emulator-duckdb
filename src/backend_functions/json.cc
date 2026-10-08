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

#include "absl/numeric/int128.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "duckdb.h"
#include "googlesql/public/functions/json_format.h"
#include "googlesql/public/functions/json_internal.h"
#include "googlesql/public/functions/to_json.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "nlohmann/json.hpp"
#include "src/backend_functions/datetime.h"
#include "src/backend_functions/json.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/bignumeric.h"
#include "src/catalog.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::backend_functions {
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

// The GoogleSQL type that the DuckDB type `type` stands for, the inverse of DuckDbType in
// src/type_mapping.cc.
absl::StatusOr<const googlesql::Type*> GoogleSqlTypeOf(duckdb_logical_type type,
                                                       googlesql::TypeFactory& factory,
                                                       const std::vector<std::string>& names,
                                                       size_t& next_name) {
  const DuckString alias(duckdb_logical_type_get_alias(type));
  if (alias != nullptr && std::string_view(alias.get()) == "JSON") {
    return googlesql::types::JsonType();
  }
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BOOLEAN:
      return googlesql::types::BoolType();
    case DUCKDB_TYPE_BIGINT:
      return googlesql::types::Int64Type();
    case DUCKDB_TYPE_DOUBLE:
      return googlesql::types::DoubleType();
    case DUCKDB_TYPE_VARCHAR:
      return googlesql::types::StringType();
    case DUCKDB_TYPE_BLOB:
      return googlesql::types::BytesType();
    case DUCKDB_TYPE_DATE:
      return googlesql::types::DateType();
    case DUCKDB_TYPE_TIME:
      return googlesql::types::TimeType();
    case DUCKDB_TYPE_TIMESTAMP:
      return googlesql::types::DatetimeType();
    case DUCKDB_TYPE_TIMESTAMP_TZ:
      return googlesql::types::TimestampType();
    case DUCKDB_TYPE_BIGNUM:
      return googlesql::types::BigNumericType();
    case DUCKDB_TYPE_DECIMAL:
      if (duckdb_decimal_width(type) == 38 && duckdb_decimal_scale(type) == 9) {
        return googlesql::types::NumericType();
      }
      break;
    case DUCKDB_TYPE_LIST: {
      LogicalType child(duckdb_list_type_child_type(type));
      const auto element = GoogleSqlTypeOf(child.get(), factory, names, next_name);
      if (!element.ok()) {
        return element.status();
      }
      const auto array = factory.MakeArrayType(*element);
      if (!array.ok()) {
        return array.status();
      }
      return *array;
    }
    case DUCKDB_TYPE_STRUCT: {
      std::vector<googlesql::StructField> fields;
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        LogicalType child(duckdb_struct_type_child_type(type, i));
        const std::string& name = names.at(next_name++);
        const auto field = GoogleSqlTypeOf(child.get(), factory, names, next_name);
        if (!field.ok()) {
          return field.status();
        }
        fields.emplace_back(name, *field);
      }
      const googlesql::StructType* type_struct = nullptr;
      if (absl::Status status = factory.MakeStructType(fields, &type_struct); !status.ok()) {
        return status;
      }
      return type_struct;
    }
    default:
      break;
  }
  return absl::UnimplementedError("Unsupported argument type for a JSON function");
}

// The value of `type`, the GoogleSQL type of the DuckDB type `duck_type`, in `row` of `vector`.
absl::StatusOr<googlesql::Value> ValueOf(duckdb_vector vector, duckdb_logical_type duck_type,
                                         const googlesql::Type* type, idx_t row) {
  uint64_t* validity = duckdb_vector_get_validity(vector);
  if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
    return googlesql::Value::Null(type);
  }
  switch (type->kind()) {
    case googlesql::TYPE_BOOL:
      return googlesql::Value::Bool(VectorElement<bool>(vector, row));
    case googlesql::TYPE_INT64:
      return googlesql::Value::Int64(VectorElement<int64_t>(vector, row));
    case googlesql::TYPE_DOUBLE:
      return googlesql::Value::Double(VectorElement<double>(vector, row));
    case googlesql::TYPE_STRING:
      return googlesql::Value::String(VectorString(vector, row));
    case googlesql::TYPE_BYTES:
      return googlesql::Value::Bytes(VectorString(vector, row));
    case googlesql::TYPE_DATE:
      return googlesql::Value::Date(VectorElement<int32_t>(vector, row));
    case googlesql::TYPE_TIME:
      return googlesql::Value::Time(TimeFromMicros(VectorElement<int64_t>(vector, row)));
    case googlesql::TYPE_DATETIME: {
      const auto datetime = DatetimeFromMicros(VectorElement<int64_t>(vector, row));
      if (!datetime.ok()) {
        return datetime.status();
      }
      return googlesql::Value::Datetime(*datetime);
    }
    case googlesql::TYPE_TIMESTAMP:
      return googlesql::Value::Timestamp(absl::FromUnixMicros(VectorElement<int64_t>(vector, row)));
    case googlesql::TYPE_NUMERIC: {
      // A DECIMAL(38, 9), stored as a 128-bit integer of units of 10^-9.
      const auto value = VectorElement<duckdb_hugeint>(vector, row);
      const absl::int128 units = absl::MakeInt128(value.upper, value.lower);
      std::string digits = absl::StrCat(units < 0 ? -units : units);
      const size_t scale = duckdb_decimal_scale(duck_type);
      digits.insert(0, scale + 1 > digits.size() ? scale + 1 - digits.size() : 0, '0');
      digits.insert(digits.size() - scale, ".");
      if (units < 0) {
        digits.insert(0, "-");
      }
      const auto numeric = googlesql::NumericValue::FromString(digits);
      return numeric.ok() ? absl::StatusOr<googlesql::Value>(googlesql::Value::Numeric(*numeric))
                          : numeric.status();
    }
    case googlesql::TYPE_BIGNUMERIC: {
      const auto bignumeric = BigNumericFromBignum(VectorString(vector, row));
      return bignumeric.ok()
                 ? absl::StatusOr<googlesql::Value>(googlesql::Value::BigNumeric(*bignumeric))
                 : bignumeric.status();
    }
    case googlesql::TYPE_JSON: {
      auto json = ParseJson(VectorString(vector, row));
      if (!json.ok()) {
        return json.status();
      }
      return googlesql::Value::Json(*std::move(json));
    }
    case googlesql::TYPE_ARRAY: {
      LogicalType child_type(duckdb_list_type_child_type(duck_type));
      const auto entry = VectorElement<duckdb_list_entry>(vector, row);
      duckdb_vector child = duckdb_list_vector_get_child(vector);
      std::vector<googlesql::Value> elements;
      for (idx_t i = 0; i < entry.length; ++i) {
        auto element =
            ValueOf(child, child_type.get(), type->AsArray()->element_type(), entry.offset + i);
        if (!element.ok()) {
          return element.status();
        }
        elements.push_back(*std::move(element));
      }
      return googlesql::Value::MakeArray(type->AsArray(), std::move(elements));
    }
    case googlesql::TYPE_STRUCT: {
      std::vector<googlesql::Value> fields;
      for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
        LogicalType child_type(duckdb_struct_type_child_type(duck_type, i));
        auto field = ValueOf(duckdb_struct_vector_get_child(vector, i), child_type.get(),
                             type->AsStruct()->field(i).type, row);
        if (!field.ok()) {
          return field.status();
        }
        fields.push_back(*std::move(field));
      }
      return googlesql::Value::MakeStruct(type->AsStruct(), std::move(fields));
    }
    default:
      return absl::UnimplementedError("Unsupported argument type for a JSON function");
  }
}

// The arguments of a function that takes values of any type, read as GoogleSQL values. The
// values' types belong to the factory, which outlives them.
class AnyArguments {
 public:
  // Only these columns carry value/name pairs; an empty list means every column does.
  explicit AnyArguments(duckdb_data_chunk input, std::initializer_list<idx_t> paired = {}) {
    if (duckdb_data_chunk_get_size(input) == 0) {
      return;
    }
    for (idx_t column = 0; column < duckdb_data_chunk_get_column_count(input); ++column) {
      duckdb_vector vector = duckdb_data_chunk_get_vector(input, column);
      LogicalType type(duckdb_vector_get_column_type(vector));
      std::vector<std::string> names;
      if (paired.size() == 0 || std::ranges::find(paired, column) != paired.end()) {
        names = nlohmann::json::parse(VectorString(duckdb_struct_vector_get_child(vector, 1), 0))
                    .get<std::vector<std::string>>();
        vector = duckdb_struct_vector_get_child(vector, 0);
        duck_types_.emplace_back(duckdb_struct_type_child_type(type.get(), 0));
      } else {
        duck_types_.push_back(std::move(type));
      }
      size_t next_name = 0;
      auto googlesql_type = GoogleSqlTypeOf(duck_types_.back().get(), factory_, names, next_name);
      if (!googlesql_type.ok()) {
        status_ = googlesql_type.status();
      }
      types_.push_back(googlesql_type.value_or(nullptr));
      vectors_.push_back(vector);
    }
  }

  [[nodiscard]] absl::StatusOr<googlesql::Value> Get(idx_t column, idx_t row) const {
    if (!status_.ok()) {
      return status_;
    }
    return ValueOf(vectors_[column], duck_types_[column].get(), types_[column], row);
  }

  [[nodiscard]] const googlesql::Type* Type(idx_t column) const {
    return column < types_.size() ? types_[column] : nullptr;
  }

 private:
  std::vector<duckdb_vector> vectors_;
  googlesql::TypeFactory factory_;
  std::vector<LogicalType> duck_types_;
  std::vector<const googlesql::Type*> types_;
  absl::Status status_;
};

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
          if (arguments_list[0].is_null()) {
            return invalid("The keys array cannot be NULL");
          }
          if (arguments_list[1].is_null()) {
            return invalid("The values array cannot be NULL");
          }
          if (arguments_list[0].num_elements() != arguments_list[1].num_elements()) {
            return invalid("The number of keys and values must match");
          }
          for (int i = 0; i < arguments_list[0].num_elements(); ++i) {
            keys.push_back(arguments_list[0].element(i));
            pointers.push_back(&arguments_list[1].element(i));
          }
        } else {
          for (idx_t i = 0; i + 1 < columns; i += 2) {
            keys.push_back(arguments_list[i]);
            pointers.push_back(&arguments_list[i + 1]);
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
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<Result>> {
            const std::string path = arguments.String(1);
            const bool standard = arguments.Bool(2);
            std::optional<googlesql::JSONValue> document;
            if constexpr (kJson) {
              auto parsed = ParseJson(arguments.String(0));
              if (!parsed.ok()) {
                return parsed.status();
              }
              document = *std::move(parsed);
              if constexpr (kExtraction == Extraction::kQuery) {
                // JSON_QUERY of JSON takes a lax JSONPath, such as "lax $.a".
                const auto lax = googlesql::functions::json_internal::IsValidAndLaxJSONPath(path);
                if (!lax.ok()) {
                  return lax.status();
                }
                if (standard && *lax) {
                  const auto iterator =
                      googlesql::functions::json_internal::StrictJSONPathIterator::Create(
                          path, /*enable_lax_mode=*/true);
                  if (!iterator.ok()) {
                    return iterator.status();
                  }
                  const auto result =
                      googlesql::functions::JsonQueryLax(document->GetConstRef(), **iterator);
                  if (!result.ok()) {
                    return result.status();
                  }
                  return result->GetConstRef().ToString();
                }
              }
            }
            const auto evaluator = googlesql::functions::JsonPathEvaluator::Create(
                path, standard, /*enable_special_character_escaping_in_values=*/true,
                /*enable_special_character_escaping_in_keys=*/true);
            if (!evaluator.ok()) {
              return evaluator.status();
            }
            if constexpr (kJson) {
              const googlesql::JSONValueConstRef json = document->GetConstRef();
              if constexpr (kExtraction == Extraction::kQuery) {
                const auto value = (*evaluator)->Extract(json);
                return value ? std::optional<std::string>(value->ToString()) : std::nullopt;
              } else if constexpr (kExtraction == Extraction::kValue) {
                return (*evaluator)->ExtractScalar(json);
              } else if constexpr (kExtraction == Extraction::kQueryArray) {
                const auto elements = (*evaluator)->ExtractArray(json);
                if (!elements) {
                  return std::nullopt;
                }
                std::vector<std::string> texts;
                std::ranges::transform(
                    *elements, std::back_inserter(texts),
                    [](googlesql::JSONValueConstRef element) { return element.ToString(); });
                return texts;
              } else {
                return (*evaluator)->ExtractStringArray(json);
              }
            } else {
              const std::string json = arguments.String(0);
              Result out;
              bool is_null = false;
              absl::Status status;
              if constexpr (kExtraction == Extraction::kQuery) {
                status = (*evaluator)->Extract(json, &out, &is_null);
              } else if constexpr (kExtraction == Extraction::kValue) {
                status = (*evaluator)->ExtractScalar(json, &out, &is_null);
              } else if constexpr (kExtraction == Extraction::kQueryArray) {
                status = (*evaluator)->ExtractArray(json, &out, &is_null);
              } else {
                status = (*evaluator)->ExtractStringArray(json, &out, &is_null);
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

// JSON_KEYS(json, max_depth, mode), as the JSON array of the keys.
void JsonKeys(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  using googlesql::functions::json_internal::JsonPathOptions;
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const int64_t max_depth =
            arguments.IsNull(1) ? std::numeric_limits<int64_t>::max() : arguments.Int(1);
        if (max_depth <= 0) {
          return absl::OutOfRangeError("max_depth must be positive.");
        }
        if (arguments.IsNull(0) || arguments.IsNull(2)) {
          return std::nullopt;
        }
        const std::string mode = arguments.String(2);
        JsonPathOptions options = JsonPathOptions::kStrict;
        if (absl::EqualsIgnoreCase(mode, "lax")) {
          options = JsonPathOptions::kLax;
        } else if (absl::EqualsIgnoreCase(mode, "lax recursive")) {
          options = JsonPathOptions::kLaxRecursive;
        } else if (!absl::EqualsIgnoreCase(mode, "strict")) {
          return absl::OutOfRangeError("Invalid JSON mode specified");
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
    LogicalType varchar(duckdb_create_logical_type(kVarchar));
    LogicalType list(duckdb_create_list_type(varchar.get()));
    for (const auto& [name, function, array] :
         std::initializer_list<std::tuple<const char*, duckdb_scalar_function_t, bool>>{
             {"bq_json_query", JsonExtract<Extraction::kQuery, false>, false},
             {"bq_json_query_json", JsonExtract<Extraction::kQuery, true>, false},
             {"bq_json_value", JsonExtract<Extraction::kValue, false>, false},
             {"bq_json_value_json", JsonExtract<Extraction::kValue, true>, false},
             {"bq_json_query_array", JsonExtract<Extraction::kQueryArray, false>, true},
             {"bq_json_query_array_json", JsonExtract<Extraction::kQueryArray, true>, true},
             {"bq_json_value_array", JsonExtract<Extraction::kValueArray, false>, true},
             {"bq_json_value_array_json", JsonExtract<Extraction::kValueArray, true>, true}}) {
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
  Register(connection, "bq_json_strip_nulls", {kVarchar, kVarchar, kBoolean, kBoolean}, kVarchar,
           JsonStripNulls, /*nulls=*/false);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
