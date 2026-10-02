#include "src/backend_functions.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "duckdb.h"
#include "googlesql/public/functions/distance.h"
#include "googlesql/public/functions/hash.h"
#include "googlesql/public/functions/json.h"
#include "googlesql/public/functions/json_internal.h"
#include "googlesql/public/functions/math.h"
#include "googlesql/public/functions/regexp.h"
#include "googlesql/public/functions/string.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/value.h"
#include "nlohmann/json.hpp"
#include "src/backend.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb {
namespace {

// The arguments of one row of a DuckDB function call.
class Arguments {
 public:
  Arguments(duckdb_data_chunk input, idx_t row) : input_(input), row_(row) {}

  [[nodiscard]] std::string String(idx_t column) const {
    return VectorString(duckdb_data_chunk_get_vector(input_, column), row_);
  }

  [[nodiscard]] int64_t Int(idx_t column) const {
    return static_cast<int64_t*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(input_, column)))[row_];
  }

  [[nodiscard]] double Double(idx_t column) const {
    return static_cast<double*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(input_, column)))[row_];
  }

  [[nodiscard]] bool Bool(idx_t column) const {
    return static_cast<bool*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(input_, column)))[row_];
  }

  [[nodiscard]] bool IsNull(idx_t column) const {
    uint64_t* validity = duckdb_vector_get_validity(duckdb_data_chunk_get_vector(input_, column));
    return validity != nullptr && !duckdb_validity_row_is_valid(validity, row_);
  }

 private:
  duckdb_data_chunk input_;
  idx_t row_;
};

void SetResult(duckdb_vector output, idx_t row, int64_t value) {
  static_cast<int64_t*>(duckdb_vector_get_data(output))[row] = value;
}

void SetResult(duckdb_vector output, idx_t row, bool value) {
  static_cast<bool*>(duckdb_vector_get_data(output))[row] = value;
}

void SetResult(duckdb_vector output, idx_t row, double value) {
  static_cast<double*>(duckdb_vector_get_data(output))[row] = value;
}

void SetResult(duckdb_vector output, idx_t row, const std::string& value) {
  duckdb_vector_assign_string_element_len(output, row, value.data(), value.size());
}

template <typename T>
void SetResult(duckdb_vector output, idx_t row, const std::optional<T>& value) {
  if (value) {
    SetResult(output, row, *value);
  } else {
    duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
  }
}

// Sets each row of `output` to what `compute` returns for the row's arguments. Unless `nulls`
// is false, a NULL argument makes a NULL result without calling `compute`. An error fails the
// query.
template <typename Compute>
void EachRow(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output,
             Compute compute, bool nulls = true) {
  const idx_t columns = duckdb_data_chunk_get_column_count(input);
  duckdb_vector_ensure_validity_writable(output);
  uint64_t* output_validity = duckdb_vector_get_validity(output);
  for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
    const Arguments arguments(input, row);
    bool null = false;
    for (idx_t column = 0; nulls && column < columns; ++column) {
      null = null || arguments.IsNull(column);
    }
    if (null) {
      duckdb_validity_set_row_invalid(output_validity, row);
      continue;
    }
    const auto result = compute(arguments);
    if (!result.ok()) {
      duckdb_scalar_function_set_error(info, std::string(result.status().message()).c_str());
      return;
    }
    SetResult(output, row, *result);
  }
}

// Turns a GoogleSQL function's out parameter and error into a StatusOr.
template <typename T>
absl::StatusOr<T> ToStatusOr(bool ok, T value, const absl::Status& error) {
  if (!ok) {
    return error;
  }
  return value;
}

// A FLOAT64 function of one argument, such as SQRT, which fails where GoogleSQL's does.
template <bool (*kFunction)(double, double*, absl::Status*)>
void Math(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    double out = 0;
    absl::Status error;
    const bool ok = kFunction(arguments.Double(0), &out, &error);
    return ToStatusOr(ok, out, error);
  });
}

// A FLOAT64 function of two arguments, such as POW.
template <bool (*kFunction)(double, double, double*, absl::Status*)>
void Math2(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    double out = 0;
    absl::Status error;
    const bool ok = kFunction(arguments.Double(0), arguments.Double(1), &out, &error);
    return ToStatusOr(ok, out, error);
  });
}

// REPEAT, which is byte for byte for STRING as for BYTES.
void Repeat(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    absl::Status error;
    const bool ok =
        googlesql::functions::Repeat(arguments.String(0), arguments.Int(1), &out, &error);
    return ToStatusOr(ok, out, error);
  });
}

void FarmFingerprint(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<int64_t> {
    return googlesql::functions::FarmFingerprint(arguments.String(0));
  });
}

void Sha512(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto hasher = googlesql::functions::Hasher::Create(googlesql::functions::Hasher::kSha512);
  EachRow(info, input, output,
          [&hasher](const Arguments& arguments) -> absl::StatusOr<std::string> {
            return hasher->Hash(arguments.String(0));
          });
}

void InitCap(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [input](const Arguments& arguments) {
    std::string out;
    absl::Status error;
    const bool ok =
        duckdb_data_chunk_get_column_count(input) == 1
            ? googlesql::functions::InitialCapitalizeDefault(arguments.String(0), &out, &error)
            : googlesql::functions::InitialCapitalize(arguments.String(0), arguments.String(1),
                                                      &out, &error);
    return ToStatusOr(ok, out, error);
  });
}

template <bool kBytes>
void EditDistance(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    return (kBytes ? googlesql::functions::EditDistanceBytes : googlesql::functions::EditDistance)(
        arguments.String(0), arguments.String(1), arguments.Int(2));
  });
}

// REGEXP_INSTR(source, regexp, position, occurrence, occurrence_position).
template <bool kBytes>
void RegexpInstr(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<int64_t> {
    const std::string pattern = arguments.String(1);
    auto regexp = kBytes ? googlesql::functions::MakeRegExpBytes(pattern)
                         : googlesql::functions::MakeRegExpUtf8(pattern);
    if (!regexp.ok()) {
      return regexp.status();
    }
    const int64_t occurrence_position = arguments.Int(4);
    if (occurrence_position != 0 && occurrence_position != 1) {
      return absl::OutOfRangeError(
          "Invalid return_position_after_match; it must be 0 or 1 (in REGEXP_INSTR)");
    }
    const std::string source = arguments.String(0);
    int64_t out = 0;
    absl::Status error;
    const bool ok = (*regexp)->Instr(
        {.input_str = source,
         .position_unit = kBytes ? googlesql::functions::RegExp::kBytes
                                 : googlesql::functions::RegExp::kUtf8Chars,
         .position = arguments.Int(2),
         .occurrence_index = arguments.Int(3),
         .return_position = occurrence_position == 0 ? googlesql::functions::RegExp::kStartOfMatch
                                                     : googlesql::functions::RegExp::kEndOfMatch,
         .out = &out},
        /*use_legacy_position_behavior=*/false, &error);
    return ToStatusOr(ok, out, error);
  });
}

// JSON goes in and out of the JSON functions as its text.
absl::StatusOr<googlesql::JSONValue> ParseJson(const std::string& text) {
  return googlesql::JSONValue::ParseJSONString(text);
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

// JSON_SET(json, path, value, create_if_missing) for one path, with the value as JSON text and
// NULL as JSON null; a NULL path or create_if_missing sets nothing.
void JsonSet(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
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
        auto value = arguments.IsNull(2) ? googlesql::JSONValue() : ParseJson(arguments.String(2));
        if (!value.ok()) {
          return value.status();
        }
        const absl::Status status = googlesql::functions::JsonSet(
            document->GetRef(), **path, googlesql::Value::Json(*std::move(value)),
            arguments.Bool(3), googlesql::LanguageOptions(), /*canonicalize_zero=*/true);
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

// JSON_OBJECT(keys, values), with the keys and the values as JSON arrays.
void JsonObject(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::string> {
        const auto invalid = [](absl::string_view message) {
          return absl::OutOfRangeError(std::string("Invalid input to JSON_OBJECT: ") +
                                       std::string(message));
        };
        if (arguments.IsNull(0)) {
          return invalid("The keys array cannot be NULL");
        }
        if (arguments.IsNull(1)) {
          return invalid("The values array cannot be NULL");
        }
        const auto keys = ParseJson(arguments.String(0));
        if (!keys.ok()) {
          return keys.status();
        }
        const auto values = ParseJson(arguments.String(1));
        if (!values.ok()) {
          return values.status();
        }
        if (keys->GetConstRef().GetArraySize() != values->GetConstRef().GetArraySize()) {
          return invalid("The number of keys and values must match");
        }
        std::vector<std::string> key_strings;
        for (const googlesql::JSONValueConstRef key : keys->GetConstRef().GetArrayElements()) {
          if (!key.IsString()) {
            return invalid("A key cannot be NULL");
          }
          key_strings.push_back(key.GetString());
        }
        std::vector<googlesql::Value> value_list;
        for (const googlesql::JSONValueConstRef value : values->GetConstRef().GetArrayElements()) {
          value_list.push_back(googlesql::Value::Json(googlesql::JSONValue::CopyFrom(value)));
        }
        const std::vector<absl::string_view> key_views(key_strings.begin(), key_strings.end());
        std::vector<const googlesql::Value*> value_pointers;
        value_pointers.reserve(value_list.size());
        for (const googlesql::Value& value : value_list) {
          value_pointers.push_back(&value);
        }
        googlesql::functions::JsonObjectBuilder builder(googlesql::LanguageOptions(),
                                                        /*canonicalize_zero=*/true);
        const auto object =
            googlesql::functions::JsonObject(key_views, absl::MakeSpan(value_pointers), builder);
        if (!object.ok()) {
          return invalid(object.status().message());
        }
        return object->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// Registers `function` under `name`, taking `parameters` and returning `result`. Unless `nulls`
// is false, DuckDB makes the result NULL for a NULL argument without calling `function`.
void Register(duckdb_connection connection, const char* name,
              std::initializer_list<duckdb_type> parameters, duckdb_type result,
              duckdb_scalar_function_t function, bool nulls = true) {
  Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), name);
  for (const duckdb_type parameter : parameters) {
    LogicalType type(duckdb_create_logical_type(parameter));
    duckdb_scalar_function_add_parameter(scalar.get(), type.get());
  }
  LogicalType type(duckdb_create_logical_type(result));
  duckdb_scalar_function_set_return_type(scalar.get(), type.get());
  duckdb_scalar_function_set_function(scalar.get(), function);
  if (!nulls) {
    duckdb_scalar_function_set_special_handling(scalar.get());
  }
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError(std::string("DuckDB failed to register ") + name);
  }
}

}  // namespace

void RegisterBackendFunctions(duckdb_database database) {
  Connection connection;
  if (duckdb_connect(database, connection.out()) == DuckDBError) {
    throw BackendError("DuckDB failed to connect");
  }
  constexpr duckdb_type kBlob = DUCKDB_TYPE_BLOB;
  constexpr duckdb_type kVarchar = DUCKDB_TYPE_VARCHAR;
  constexpr duckdb_type kBigint = DUCKDB_TYPE_BIGINT;
  constexpr duckdb_type kBoolean = DUCKDB_TYPE_BOOLEAN;
  constexpr duckdb_type kDouble = DUCKDB_TYPE_DOUBLE;
  namespace fn = googlesql::functions;
  for (const auto& [name, function] :
       std::initializer_list<std::pair<const char*, duckdb_scalar_function_t>>{
           {"bq_sqrt", Math<fn::Sqrt<double>>},
           {"bq_cbrt", Math<fn::Cbrt<double>>},
           {"bq_exp", Math<fn::Exp<double>>},
           {"bq_ln", Math<fn::NaturalLogarithm<double>>},
           {"bq_log10", Math<fn::DecimalLogarithm<double>>},
           {"bq_sin", Math<fn::Sin<double>>},
           {"bq_cos", Math<fn::Cos<double>>},
           {"bq_tan", Math<fn::Tan<double>>},
           {"bq_asin", Math<fn::Asin<double>>},
           {"bq_acos", Math<fn::Acos<double>>},
           {"bq_sinh", Math<fn::Sinh<double>>},
           {"bq_cosh", Math<fn::Cosh<double>>},
           {"bq_acosh", Math<fn::Acosh<double>>},
           {"bq_atanh", Math<fn::Atanh<double>>},
           {"bq_csc", Math<fn::Csc<double>>},
           {"bq_sec", Math<fn::Sec<double>>},
           {"bq_cot", Math<fn::Cot<double>>},
           {"bq_csch", Math<fn::Csch<double>>},
           {"bq_sech", Math<fn::Sech<double>>},
           {"bq_coth", Math<fn::Coth<double>>},
       }) {
    Register(connection.get(), name, {kDouble}, kDouble, function);
  }
  Register(connection.get(), "bq_repeat", {kVarchar, kBigint}, kVarchar, Repeat);
  Register(connection.get(), "bq_repeat_bytes", {kBlob, kBigint}, kBlob, Repeat);
  Register(connection.get(), "bq_pow", {kDouble, kDouble}, kDouble, Math2<fn::Pow<double>>);
  Register(connection.get(), "bq_log", {kDouble, kDouble}, kDouble, Math2<fn::Logarithm<double>>);
  Register(connection.get(), "bq_farm_fingerprint", {kBlob}, kBigint, FarmFingerprint);
  Register(connection.get(), "bq_sha512", {kBlob}, kBlob, Sha512);
  Register(connection.get(), "bq_initcap", {kVarchar}, kVarchar, InitCap);
  Register(connection.get(), "bq_initcap_delimiters", {kVarchar, kVarchar}, kVarchar, InitCap);
  Register(connection.get(), "bq_edit_distance", {kVarchar, kVarchar, kBigint}, kBigint,
           EditDistance<false>);
  Register(connection.get(), "bq_edit_distance_bytes", {kBlob, kBlob, kBigint}, kBigint,
           EditDistance<true>);
  Register(connection.get(), "bq_regexp_instr", {kVarchar, kVarchar, kBigint, kBigint, kBigint},
           kBigint, RegexpInstr<false>);
  Register(connection.get(), "bq_regexp_instr_bytes", {kBlob, kBlob, kBigint, kBigint, kBigint},
           kBigint, RegexpInstr<true>);
  Register(connection.get(), "bq_lax_bool", {kVarchar}, kBoolean,
           LaxConvert<bool, googlesql::functions::LaxConvertJsonToBool>);
  Register(connection.get(), "bq_lax_int64", {kVarchar}, kBigint,
           LaxConvert<int64_t, googlesql::functions::LaxConvertJsonToInt64>);
  Register(connection.get(), "bq_lax_float64", {kVarchar}, kDouble,
           LaxConvert<double, googlesql::functions::LaxConvertJsonToFloat64>);
  Register(connection.get(), "bq_lax_string", {kVarchar}, kVarchar,
           LaxConvert<std::string, googlesql::functions::LaxConvertJsonToString>);
  Register(connection.get(), "bq_json_keys", {kVarchar, kBigint, kVarchar}, kVarchar, JsonKeys,
           /*nulls=*/false);
  Register(connection.get(), "bq_json_remove", {kVarchar, kVarchar}, kVarchar, JsonRemove,
           /*nulls=*/false);
  Register(connection.get(), "bq_json_set", {kVarchar, kVarchar, kVarchar, kBoolean}, kVarchar,
           JsonSet, /*nulls=*/false);
  Register(connection.get(), "bq_json_strip_nulls", {kVarchar, kVarchar, kBoolean, kBoolean},
           kVarchar, JsonStripNulls, /*nulls=*/false);
  Register(connection.get(), "bq_json_object", {kVarchar, kVarchar}, kVarchar, JsonObject,
           /*nulls=*/false);
}

}  // namespace bigquery_emulator_duckdb
