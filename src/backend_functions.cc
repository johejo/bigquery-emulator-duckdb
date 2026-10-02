#include "src/backend_functions.h"

#include <cstdint>
#include <initializer_list>
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
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/functions/distance.h"
#include "googlesql/public/functions/hash.h"
#include "googlesql/public/functions/json.h"
#include "googlesql/public/functions/json_format.h"
#include "googlesql/public/functions/json_internal.h"
#include "googlesql/public/functions/math.h"
#include "googlesql/public/functions/normalize_mode.pb.h"
#include "googlesql/public/functions/parse_date_time.h"
#include "googlesql/public/functions/regexp.h"
#include "googlesql/public/functions/string.h"
#include "googlesql/public/functions/string_format.h"
#include "googlesql/public/functions/to_json.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "nlohmann/json.hpp"
#include "src/backend.h"
#include "src/catalog.h"
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

  // A DATE as days since the epoch.
  [[nodiscard]] int32_t Date(idx_t column) const {
    return static_cast<int32_t*>(
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

  [[nodiscard]] idx_t Row() const { return row_; }

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

// A DATE as days since the epoch.
void SetResult(duckdb_vector output, idx_t row, int32_t value) {
  static_cast<int32_t*>(duckdb_vector_get_data(output))[row] = value;
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

// An ARRAY<STRING> or ARRAY<BYTES>, appended to the list's child vector, where a null element
// is NULL.
template <typename Element>
void SetResult(duckdb_vector output, idx_t row, const std::vector<Element>& values) {
  const idx_t offset = duckdb_list_vector_get_size(output);
  duckdb_list_vector_reserve(output, offset + values.size());
  duckdb_vector child = duckdb_list_vector_get_child(output);
  for (idx_t i = 0; i < values.size(); ++i) {
    const std::optional<std::string>& value = values[i];
    if (value) {
      duckdb_vector_assign_string_element_len(child, offset + i, value->data(), value->size());
    } else {
      duckdb_vector_ensure_validity_writable(child);
      duckdb_validity_set_row_invalid(duckdb_vector_get_validity(child), offset + i);
    }
  }
  duckdb_list_vector_set_size(output, offset + values.size());
  static_cast<duckdb_list_entry*>(duckdb_vector_get_data(output))[row] = {offset, values.size()};
}

template <typename T>
void SetResult(duckdb_vector output, idx_t row, const std::optional<T>& value) {
  if (value) {
    SetResult(output, row, *value);
  } else {
    duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
  }
}

// The DuckDB type of each argument column.
std::vector<duckdb_type> ColumnTypes(duckdb_data_chunk input) {
  std::vector<duckdb_type> types;
  for (idx_t column = 0; column < duckdb_data_chunk_get_column_count(input); ++column) {
    LogicalType type(duckdb_vector_get_column_type(duckdb_data_chunk_get_vector(input, column)));
    types.push_back(duckdb_get_type_id(type.get()));
  }
  return types;
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

// The parameters of a GoogleSQL string function: its arguments, then an out parameter and an
// error.
template <typename Function>
struct Signature;

template <typename... Parameters>
struct Signature<bool (*)(Parameters...)> {
  static constexpr std::size_t kArity = sizeof...(Parameters) - 2;
  template <std::size_t I>
  using Parameter = std::tuple_element_t<I, std::tuple<Parameters...>>;
  using Out = std::remove_pointer_t<Parameter<kArity>>;
};

// A STRING or BYTES argument as a string_view parameter, or an INT64 one.
template <typename T>
auto Argument(const Arguments& arguments, idx_t column) {
  if constexpr (std::is_same_v<T, int64_t>) {
    return arguments.Int(column);
  } else {
    return arguments.String(column);
  }
}

// A string_view result points into the arguments, which do not outlive the row.
template <typename T>
auto Owned(T value) {
  if constexpr (std::is_same_v<T, absl::string_view>) {
    return std::string(value);
  } else {
    return value;
  }
}

// The GoogleSQL function `kFunction` of STRING, BYTES and INT64 arguments, such as LowerUtf8,
// taking the columns in order.
template <auto kFunction>
void Apply(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  using S = Signature<decltype(kFunction)>;
  EachRow(info, input, output, [](const Arguments& arguments) {
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      const std::tuple values{Argument<typename S::template Parameter<I>>(arguments, I)...};
      typename S::Out out{};
      absl::Status error;
      const bool ok = kFunction(std::get<I>(values)..., &out, &error);
      return ToStatusOr(ok, Owned(std::move(out)), error);
    }(std::make_index_sequence<S::kArity>());
  });
}

// NORMALIZE and NORMALIZE_AND_CASEFOLD, with the mode by its name, such as NFKC.
template <bool kCasefold>
void Normalize(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    googlesql::functions::NormalizeMode mode;
    if (!googlesql::functions::NormalizeMode_Parse(arguments.String(1), &mode)) {
      return absl::OutOfRangeError("Invalid normalize mode");
    }
    std::string out;
    absl::Status error;
    const bool ok =
        googlesql::functions::Normalize(arguments.String(0), mode, kCasefold, &out, &error);
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

// Sets each row to what `compute` returns for the row's arguments and its regular expression,
// the second argument, which GoogleSQL compiles as UTF-8 for STRING or as bytes for BYTES.
template <bool kBytes, typename Compute>
void WithRegExp(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output,
                Compute compute) {
  EachRow(info, input, output, [&compute](const Arguments& arguments) {
    const std::string pattern = arguments.String(1);
    const auto regexp = kBytes ? googlesql::functions::MakeRegExpBytes(pattern)
                               : googlesql::functions::MakeRegExpUtf8(pattern);
    using Result = decltype(compute(**regexp, arguments));
    if (!regexp.ok()) {
      return Result(regexp.status());
    }
    return compute(**regexp, arguments);
  });
}

constexpr googlesql::functions::RegExp::PositionUnit PositionUnit(bool bytes) {
  return bytes ? googlesql::functions::RegExp::kBytes : googlesql::functions::RegExp::kUtf8Chars;
}

template <bool kBytes>
void RegexpContains(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  WithRegExp<kBytes>(info, input, output,
                     [](const googlesql::functions::RegExp& regexp, const Arguments& arguments) {
                       bool out = false;
                       absl::Status error;
                       const bool ok = regexp.Contains(arguments.String(0), &out, &error);
                       return ToStatusOr(ok, out, error);
                     });
}

// REGEXP_REPLACE(source, regexp, replacement).
template <bool kBytes>
void RegexpReplace(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  WithRegExp<kBytes>(info, input, output,
                     [](const googlesql::functions::RegExp& regexp, const Arguments& arguments) {
                       std::string out;
                       absl::Status error;
                       const bool ok =
                           regexp.Replace(arguments.String(0), arguments.String(2), &out, &error);
                       return ToStatusOr(ok, out, error);
                     });
}

// REGEXP_EXTRACT(source, regexp, position, occurrence), which is NULL without a match or when
// the capturing group takes no part in it.
template <bool kBytes>
void RegexpExtract(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  WithRegExp<kBytes>(
      info, input, output,
      [](const googlesql::functions::RegExp& regexp,
         const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const std::string source = arguments.String(0);
        absl::string_view out;
        bool is_null = true;
        absl::Status error;
        if (!regexp.Extract(source, PositionUnit(kBytes), arguments.Int(2), arguments.Int(3),
                            /*use_legacy_position_behavior=*/false, &out, &is_null, &error)) {
          return error;
        }
        if (is_null) {
          return std::nullopt;
        }
        return std::string(out);
      });
}

// REGEXP_EXTRACT_ALL(source, regexp).
template <bool kBytes>
void RegexpExtractAll(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  WithRegExp<kBytes>(info, input, output,
                     [](const googlesql::functions::RegExp& regexp,
                        const Arguments& arguments) -> absl::StatusOr<std::vector<std::string>> {
                       const std::string source = arguments.String(0);
                       auto matches = regexp.CreateExtractAllIterator(source);
                       std::vector<std::string> out;
                       absl::string_view match;
                       absl::Status error;
                       while (matches.Next(&match, &error)) {
                         out.emplace_back(match);
                       }
                       if (!error.ok()) {
                         return error;
                       }
                       return out;
                     });
}

// REGEXP_INSTR(source, regexp, position, occurrence, occurrence_position).
template <bool kBytes>
void RegexpInstr(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  WithRegExp<kBytes>(
      info, input, output,
      [](const googlesql::functions::RegExp& regexp,
         const Arguments& arguments) -> absl::StatusOr<int64_t> {
        const int64_t occurrence_position = arguments.Int(4);
        if (occurrence_position != 0 && occurrence_position != 1) {
          return absl::OutOfRangeError(
              "Invalid return_position_after_match; it must be 0 or 1 (in REGEXP_INSTR)");
        }
        const std::string source = arguments.String(0);
        int64_t out = 0;
        absl::Status error;
        const bool ok =
            regexp.Instr({.input_str = source,
                          .position_unit = PositionUnit(kBytes),
                          .position = arguments.Int(2),
                          .occurrence_index = arguments.Int(3),
                          .return_position = occurrence_position == 0
                                                 ? googlesql::functions::RegExp::kStartOfMatch
                                                 : googlesql::functions::RegExp::kEndOfMatch,
                          .out = &out},
                         /*use_legacy_position_behavior=*/false, &error);
        return ToStatusOr(ok, out, error);
      });
}

// DuckDB keeps a TIME, DATETIME or TIMESTAMP as microseconds since midnight or the epoch, a
// DATETIME as the civil time in UTC.
googlesql::TimeValue TimeFromMicros(int64_t micros) {
  constexpr int64_t kPerSecond = 1000000;
  const int64_t seconds = micros / kPerSecond;
  return googlesql::TimeValue::FromHMSAndMicros(
      static_cast<int32_t>(seconds / 3600), static_cast<int32_t>(seconds / 60 % 60),
      static_cast<int32_t>(seconds % 60), static_cast<int32_t>(micros % kPerSecond));
}

int64_t MicrosFromTime(const googlesql::TimeValue& time) {
  return ((((int64_t{time.Hour()} * 60) + time.Minute()) * 60) + time.Second()) * 1000000 +
         time.Microseconds();
}

absl::StatusOr<googlesql::DatetimeValue> DatetimeFromMicros(int64_t micros) {
  googlesql::DatetimeValue datetime;
  const absl::Status status = googlesql::functions::ConvertTimestampToDatetime(
      absl::FromUnixMicros(micros), absl::UTCTimeZone(), &datetime);
  return ToStatusOr(status.ok(), datetime, status);
}

absl::StatusOr<int64_t> MicrosFromDatetime(const googlesql::DatetimeValue& datetime) {
  absl::Time time;
  const absl::Status status =
      googlesql::functions::ConvertDatetimeToTimestamp(datetime, absl::UTCTimeZone(), &time);
  return ToStatusOr(status.ok(), absl::ToUnixMicros(time), status);
}

// BigQuery expands %Q to the quarter and %J to the ISO day of the year.
constexpr googlesql::functions::FormatDateTimestampOptions kFormatOptions = {.expand_Q = true,
                                                                             .expand_J = true};

// FORMAT_DATE(format, date).
void FormatDate(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    const absl::Status status = googlesql::functions::FormatDateToString(
        arguments.String(0), int64_t{arguments.Date(1)}, kFormatOptions, &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// FORMAT_DATETIME(format, datetime).
void FormatDatetime(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto datetime = DatetimeFromMicros(arguments.Int(1));
    if (!datetime.ok()) {
      return datetime.status();
    }
    std::string out;
    const absl::Status status = googlesql::functions::FormatDatetimeToStringWithOptions(
        arguments.String(0), *datetime, kFormatOptions, &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// FORMAT_TIME(format, time).
void FormatTime(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    const absl::Status status = googlesql::functions::FormatTimeToString(
        arguments.String(0), TimeFromMicros(arguments.Int(1)), &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// FORMAT_TIMESTAMP(format, timestamp, time_zone).
void FormatTimestamp(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    const absl::Status status = googlesql::functions::FormatTimestampToString(
        arguments.String(0), arguments.Int(1), arguments.String(2), kFormatOptions, &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// PARSE_DATE(format, string).
void ParseDate(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    int32_t out = 0;
    const absl::Status status = googlesql::functions::ParseStringToDate(
        arguments.String(0), arguments.String(1), /*parse_version2=*/true, &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// PARSE_DATETIME(format, string).
void ParseDatetime(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<int64_t> {
    googlesql::DatetimeValue out;
    absl::Status status = googlesql::functions::ParseStringToDatetime(
        arguments.String(0), arguments.String(1), googlesql::functions::kMicroseconds,
        /*parse_version2=*/true, &out);
    if (!status.ok()) {
      return status;
    }
    return MicrosFromDatetime(out);
  });
}

// PARSE_TIME(format, string).
void ParseTime(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<int64_t> {
    googlesql::TimeValue out;
    absl::Status status = googlesql::functions::ParseStringToTime(
        arguments.String(0), arguments.String(1), googlesql::functions::kMicroseconds, &out);
    if (!status.ok()) {
      return status;
    }
    return MicrosFromTime(out);
  });
}

// PARSE_TIMESTAMP(format, string, time_zone), with the time zone for a string without one.
void ParseTimestamp(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    int64_t out = 0;
    const absl::Status status = googlesql::functions::ParseStringToTimestamp(
        arguments.String(0), arguments.String(1), arguments.String(2), /*parse_version2=*/true,
        &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// A FORMAT argument as the GoogleSQL value of the type its DuckDB type stands for.
absl::StatusOr<googlesql::Value> FormatArgument(const Arguments& arguments, idx_t column,
                                                duckdb_type type) {
  const bool null = arguments.IsNull(column);
  switch (type) {
    case DUCKDB_TYPE_VARCHAR:
      return null ? googlesql::Value::NullString()
                  : googlesql::Value::String(arguments.String(column));
    case DUCKDB_TYPE_BLOB:
      return null ? googlesql::Value::NullBytes()
                  : googlesql::Value::Bytes(arguments.String(column));
    case DUCKDB_TYPE_BIGINT:
      return null ? googlesql::Value::NullInt64() : googlesql::Value::Int64(arguments.Int(column));
    case DUCKDB_TYPE_DOUBLE:
      return null ? googlesql::Value::NullDouble()
                  : googlesql::Value::Double(arguments.Double(column));
    case DUCKDB_TYPE_BOOLEAN:
      return null ? googlesql::Value::NullBool() : googlesql::Value::Bool(arguments.Bool(column));
    case DUCKDB_TYPE_DATE:
      return null ? googlesql::Value::NullDate() : googlesql::Value::Date(arguments.Date(column));
    case DUCKDB_TYPE_TIME:
      return null ? googlesql::Value::NullTime()
                  : googlesql::Value::Time(TimeFromMicros(arguments.Int(column)));
    case DUCKDB_TYPE_TIMESTAMP: {
      if (null) {
        return googlesql::Value::NullDatetime();
      }
      const auto datetime = DatetimeFromMicros(arguments.Int(column));
      if (!datetime.ok()) {
        return datetime.status();
      }
      return googlesql::Value::Datetime(*datetime);
    }
    case DUCKDB_TYPE_TIMESTAMP_TZ:
      return null ? googlesql::Value::NullTimestamp()
                  : googlesql::Value::Timestamp(absl::FromUnixMicros(arguments.Int(column)));
    default:
      return absl::UnimplementedError("Unsupported argument type for FORMAT");
  }
}

// FORMAT(format, values...), which is NULL for a NULL format and where GoogleSQL says so for
// a NULL value.
void Format(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const std::vector<duckdb_type> types = ColumnTypes(input);
  EachRow(
      info, input, output,
      [&types](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        std::vector<googlesql::Value> values;
        for (idx_t column = 1; column < types.size(); ++column) {
          auto value = FormatArgument(arguments, column, types[column]);
          if (!value.ok()) {
            return value.status();
          }
          values.push_back(*std::move(value));
        }
        std::string out;
        bool is_null = false;
        absl::Status status = googlesql::functions::StringFormatUtf8(
            arguments.String(0), values, googlesql::PRODUCT_EXTERNAL, &out, &is_null,
            /*canonicalize_zero=*/true, /*use_external_float32=*/true);
        if (!status.ok()) {
          return status;
        }
        return is_null ? std::nullopt : std::optional<std::string>(out);
      },
      /*nulls=*/false);
}

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
// src/type_mapping.cc, with NUMERIC and BIGNUMERIC told apart by their scale.
absl::StatusOr<const googlesql::Type*> GoogleSqlTypeOf(duckdb_logical_type type,
                                                       googlesql::TypeFactory& factory) {
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
    case DUCKDB_TYPE_DECIMAL:
      if (duckdb_decimal_width(type) == 38 && duckdb_decimal_scale(type) == 9) {
        return googlesql::types::NumericType();
      }
      if (duckdb_decimal_width(type) == 38 && duckdb_decimal_scale(type) == 19) {
        return googlesql::types::BigNumericType();
      }
      break;
    case DUCKDB_TYPE_LIST: {
      LogicalType child(duckdb_list_type_child_type(type));
      const auto element = GoogleSqlTypeOf(child.get(), factory);
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
        const DuckString name(duckdb_struct_type_child_name(type, i));
        const auto field = GoogleSqlTypeOf(child.get(), factory);
        if (!field.ok()) {
          return field.status();
        }
        fields.emplace_back(name.get(), *field);
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
    case googlesql::TYPE_NUMERIC:
    case googlesql::TYPE_BIGNUMERIC: {
      // Both are DECIMAL(38, s), stored as a 128-bit integer of units of 10^-s.
      const auto value = VectorElement<duckdb_hugeint>(vector, row);
      const absl::int128 units = absl::MakeInt128(value.upper, value.lower);
      std::string digits = absl::StrCat(units < 0 ? -units : units);
      const size_t scale = duckdb_decimal_scale(duck_type);
      digits.insert(0, scale + 1 > digits.size() ? scale + 1 - digits.size() : 0, '0');
      digits.insert(digits.size() - scale, ".");
      if (units < 0) {
        digits.insert(0, "-");
      }
      if (type->IsNumericType()) {
        const auto numeric = googlesql::NumericValue::FromString(digits);
        return numeric.ok() ? absl::StatusOr<googlesql::Value>(googlesql::Value::Numeric(*numeric))
                            : numeric.status();
      }
      const auto numeric = googlesql::BigNumericValue::FromString(digits);
      return numeric.ok() ? absl::StatusOr<googlesql::Value>(googlesql::Value::BigNumeric(*numeric))
                          : numeric.status();
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
      const duckdb_list_entry entry = VectorElement<duckdb_list_entry>(vector, row);
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
  explicit AnyArguments(duckdb_data_chunk input) : input_(input) {
    for (idx_t column = 0; column < duckdb_data_chunk_get_column_count(input); ++column) {
      LogicalType type(duckdb_vector_get_column_type(duckdb_data_chunk_get_vector(input, column)));
      auto googlesql_type = GoogleSqlTypeOf(type.get(), factory_);
      if (!googlesql_type.ok()) {
        status_ = googlesql_type.status();
      }
      types_.push_back(googlesql_type.value_or(nullptr));
      duck_types_.push_back(std::move(type));
    }
  }

  [[nodiscard]] absl::StatusOr<googlesql::Value> Get(idx_t column, idx_t row) const {
    if (!status_.ok()) {
      return status_;
    }
    return ValueOf(duckdb_data_chunk_get_vector(input_, column), duck_types_[column].get(),
                   types_[column], row);
  }

  [[nodiscard]] const googlesql::Type* Type(idx_t column) const { return types_[column]; }

 private:
  duckdb_data_chunk input_;
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
  const AnyArguments values(input);
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
  const AnyArguments values(input);
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
                for (const googlesql::JSONValueConstRef element : *elements) {
                  texts.push_back(element.ToString());
                }
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
        if (!json.IsArray() || index < 0 || static_cast<uint64_t>(index) >= json.GetArraySize()) {
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
  const AnyArguments values(input);
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

// Registers `function` under `name`, taking `parameters` and returning `result`. Unless `nulls`
// is false, DuckDB makes the result NULL for a NULL argument without calling `function`.
void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt) {
  Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), name);
  for (const duckdb_type parameter : parameters) {
    LogicalType type(duckdb_create_logical_type(parameter));
    duckdb_scalar_function_add_parameter(scalar.get(), type.get());
  }
  if (varargs) {
    LogicalType type(duckdb_create_logical_type(*varargs));
    duckdb_scalar_function_set_varargs(scalar.get(), type.get());
  }
  duckdb_scalar_function_set_return_type(scalar.get(), result);
  duckdb_scalar_function_set_function(scalar.get(), function);
  if (!nulls) {
    duckdb_scalar_function_set_special_handling(scalar.get());
  }
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError(std::string("DuckDB failed to register ") + name);
  }
}

void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt) {
  LogicalType type(duckdb_create_logical_type(result));
  Register(connection, name, parameters, type.get(), function, nulls, varargs);
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
  // String functions, where DuckDB differs in case mapping, whitespace, errors and limits, or
  // has no BYTES overload.
  for (const auto& [name, parameters, result, function] :
       std::initializer_list<std::tuple<const char*, std::vector<duckdb_type>, duckdb_type,
                                        duckdb_scalar_function_t>>{
           {"bq_repeat", {kVarchar, kBigint}, kVarchar, Apply<fn::Repeat>},
           {"bq_repeat_bytes", {kBlob, kBigint}, kBlob, Apply<fn::Repeat>},
           {"bq_lower", {kVarchar}, kVarchar, Apply<fn::LowerUtf8>},
           {"bq_lower_bytes", {kBlob}, kBlob, Apply<fn::LowerBytes>},
           {"bq_upper", {kVarchar}, kVarchar, Apply<fn::UpperUtf8>},
           {"bq_upper_bytes", {kBlob}, kBlob, Apply<fn::UpperBytes>},
           {"bq_reverse", {kVarchar}, kVarchar, Apply<fn::ReverseUtf8>},
           {"bq_reverse_bytes", {kBlob}, kBlob, Apply<fn::ReverseBytes>},
           {"bq_replace", {kVarchar, kVarchar, kVarchar}, kVarchar, Apply<fn::ReplaceUtf8>},
           {"bq_replace_bytes", {kBlob, kBlob, kBlob}, kBlob, Apply<fn::ReplaceBytes>},
           {"bq_translate", {kVarchar, kVarchar, kVarchar}, kVarchar, Apply<fn::TranslateUtf8>},
           {"bq_translate_bytes", {kBlob, kBlob, kBlob}, kBlob, Apply<fn::TranslateBytes>},
           {"bq_starts_with_bytes", {kBlob, kBlob}, kBoolean, Apply<fn::StartsWithBytes>},
           {"bq_ends_with_bytes", {kBlob, kBlob}, kBoolean, Apply<fn::EndsWithBytes>},
           {"bq_ascii", {kVarchar}, kBigint, Apply<fn::FirstCharOfStringToASCII>},
           {"bq_ascii_bytes", {kBlob}, kBigint, Apply<fn::FirstByteOfBytesToASCII>},
           {"bq_instr",
            {kVarchar, kVarchar, kBigint, kBigint},
            kBigint,
            Apply<fn::StrPosOccurrenceUtf8>},
           {"bq_instr_bytes",
            {kBlob, kBlob, kBigint, kBigint},
            kBigint,
            Apply<fn::StrPosOccurrenceBytes>},
           {"bq_left", {kVarchar, kBigint}, kVarchar, Apply<fn::LeftUtf8>},
           {"bq_left_bytes", {kBlob, kBigint}, kBlob, Apply<fn::LeftBytes>},
           {"bq_right", {kVarchar, kBigint}, kVarchar, Apply<fn::RightUtf8>},
           {"bq_right_bytes", {kBlob, kBigint}, kBlob, Apply<fn::RightBytes>},
           {"bq_substr_bytes", {kBlob, kBigint, kBigint}, kBlob, Apply<fn::SubstrWithLengthBytes>},
           {"bq_lpad", {kVarchar, kBigint, kVarchar}, kVarchar, Apply<fn::LeftPadUtf8>},
           {"bq_lpad_bytes", {kBlob, kBigint, kBlob}, kBlob, Apply<fn::LeftPadBytes>},
           {"bq_rpad", {kVarchar, kBigint, kVarchar}, kVarchar, Apply<fn::RightPadUtf8>},
           {"bq_rpad_bytes", {kBlob, kBigint, kBlob}, kBlob, Apply<fn::RightPadBytes>},
           {"bq_trim", {kVarchar}, kVarchar, Apply<fn::TrimSpacesUtf8>},
           {"bq_trim_chars", {kVarchar, kVarchar}, kVarchar, Apply<fn::TrimUtf8>},
           {"bq_trim_bytes", {kBlob, kBlob}, kBlob, Apply<fn::TrimBytes>},
           {"bq_ltrim", {kVarchar}, kVarchar, Apply<fn::LeftTrimSpacesUtf8>},
           {"bq_ltrim_chars", {kVarchar, kVarchar}, kVarchar, Apply<fn::LeftTrimUtf8>},
           {"bq_ltrim_bytes", {kBlob, kBlob}, kBlob, Apply<fn::LeftTrimBytes>},
           {"bq_rtrim", {kVarchar}, kVarchar, Apply<fn::RightTrimSpacesUtf8>},
           {"bq_rtrim_chars", {kVarchar, kVarchar}, kVarchar, Apply<fn::RightTrimUtf8>},
           {"bq_rtrim_bytes", {kBlob, kBlob}, kBlob, Apply<fn::RightTrimBytes>},
           {"bq_normalize", {kVarchar, kVarchar}, kVarchar, Normalize<false>},
           {"bq_normalize_and_casefold", {kVarchar, kVarchar}, kVarchar, Normalize<true>},
       }) {
    Register(connection.get(), name, parameters, result, function);
  }
  {
    LogicalType blob(duckdb_create_logical_type(kBlob));
    LogicalType list(duckdb_create_list_type(blob.get()));
    Register(
        connection.get(), "bq_split_bytes", {kBlob, kBlob}, list.get(),
        Apply<static_cast<bool (*)(absl::string_view, absl::string_view, std::vector<std::string>*,
                                   absl::Status*)>(fn::SplitBytes)>);
  }
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
  Register(connection.get(), "bq_regexp_contains", {kVarchar, kVarchar}, kBoolean,
           RegexpContains<false>);
  Register(connection.get(), "bq_regexp_contains_bytes", {kBlob, kBlob}, kBoolean,
           RegexpContains<true>);
  Register(connection.get(), "bq_regexp_replace", {kVarchar, kVarchar, kVarchar}, kVarchar,
           RegexpReplace<false>);
  Register(connection.get(), "bq_regexp_replace_bytes", {kBlob, kBlob, kBlob}, kBlob,
           RegexpReplace<true>);
  Register(connection.get(), "bq_regexp_extract", {kVarchar, kVarchar, kBigint, kBigint}, kVarchar,
           RegexpExtract<false>);
  Register(connection.get(), "bq_regexp_extract_bytes", {kBlob, kBlob, kBigint, kBigint}, kBlob,
           RegexpExtract<true>);
  for (const auto& [name, type, function] :
       std::initializer_list<std::tuple<const char*, duckdb_type, duckdb_scalar_function_t>>{
           {"bq_regexp_extract_all", kVarchar, RegexpExtractAll<false>},
           {"bq_regexp_extract_all_bytes", kBlob, RegexpExtractAll<true>}}) {
    LogicalType element(duckdb_create_logical_type(type));
    LogicalType list(duckdb_create_list_type(element.get()));
    Register(connection.get(), name, {type, type}, list.get(), function);
  }
  constexpr duckdb_type kDate = DUCKDB_TYPE_DATE;
  constexpr duckdb_type kTime = DUCKDB_TYPE_TIME;
  constexpr duckdb_type kDatetime = DUCKDB_TYPE_TIMESTAMP;
  constexpr duckdb_type kTimestamp = DUCKDB_TYPE_TIMESTAMP_TZ;
  Register(connection.get(), "bq_format_date", {kVarchar, kDate}, kVarchar, FormatDate);
  Register(connection.get(), "bq_format_datetime", {kVarchar, kDatetime}, kVarchar, FormatDatetime);
  Register(connection.get(), "bq_format_time", {kVarchar, kTime}, kVarchar, FormatTime);
  Register(connection.get(), "bq_format_timestamp", {kVarchar, kTimestamp, kVarchar}, kVarchar,
           FormatTimestamp);
  Register(connection.get(), "bq_parse_date", {kVarchar, kVarchar}, kDate, ParseDate);
  Register(connection.get(), "bq_parse_datetime", {kVarchar, kVarchar}, kDatetime, ParseDatetime);
  Register(connection.get(), "bq_parse_time", {kVarchar, kVarchar}, kTime, ParseTime);
  Register(connection.get(), "bq_parse_timestamp", {kVarchar, kVarchar, kVarchar}, kTimestamp,
           ParseTimestamp);
  Register(connection.get(), "bq_format", {kVarchar}, kVarchar, Format, /*nulls=*/false,
           DUCKDB_TYPE_ANY);
  Register(connection.get(), "bq_lax_bool", {kVarchar}, kBoolean,
           LaxConvert<bool, googlesql::functions::LaxConvertJsonToBool>);
  Register(connection.get(), "bq_lax_int64", {kVarchar}, kBigint,
           LaxConvert<int64_t, googlesql::functions::LaxConvertJsonToInt64>);
  Register(connection.get(), "bq_lax_float64", {kVarchar}, kDouble,
           LaxConvert<double, googlesql::functions::LaxConvertJsonToFloat64>);
  Register(connection.get(), "bq_lax_string", {kVarchar}, kVarchar,
           LaxConvert<std::string, googlesql::functions::LaxConvertJsonToString>);
  Register(connection.get(), "bq_json_bool", {kVarchar}, kBoolean,
           ConvertJson<bool, googlesql::functions::ConvertJsonToBool>);
  Register(connection.get(), "bq_json_int64", {kVarchar}, kBigint,
           ConvertJson<int64_t, googlesql::functions::ConvertJsonToInt64>);
  Register(connection.get(), "bq_json_string", {kVarchar}, kVarchar,
           ConvertJson<std::string, googlesql::functions::ConvertJsonToString>);
  Register(connection.get(), "bq_json_float64", {kVarchar, kVarchar}, kDouble, JsonToFloat64);
  Register(connection.get(), "bq_json_type", {kVarchar}, kVarchar, JsonType);
  Register(connection.get(), "bq_parse_json", {kVarchar, kVarchar}, kVarchar, ParseJsonFunction);
  Register(connection.get(), "bq_json_field", {kVarchar, kVarchar}, kVarchar, JsonField);
  Register(connection.get(), "bq_json_element", {kVarchar, kBigint}, kVarchar, JsonElement);
  constexpr duckdb_type kAny = DUCKDB_TYPE_ANY;
  Register(connection.get(), "bq_to_json_string", {kAny, kBoolean}, kVarchar, ToJsonString,
           /*nulls=*/false);
  Register(connection.get(), "bq_to_json", {kAny, kBoolean}, kVarchar, ToJson, /*nulls=*/false);
  Register(connection.get(), "bq_json_array", {}, kVarchar, JsonArray, /*nulls=*/false, kAny);
  Register(connection.get(), "bq_json_object", {}, kVarchar, JsonObject, /*nulls=*/false, kAny);
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
      Register(connection.get(), name, {kVarchar, kVarchar, kBoolean},
               array ? list.get() : varchar.get(), function);
    }
  }
  Register(connection.get(), "bq_json_keys", {kVarchar, kBigint, kVarchar}, kVarchar, JsonKeys,
           /*nulls=*/false);
  Register(connection.get(), "bq_json_remove", {kVarchar, kVarchar}, kVarchar, JsonRemove,
           /*nulls=*/false);
  Register(connection.get(), "bq_json_set", {kVarchar, kVarchar, kAny, kBoolean}, kVarchar, JsonSet,
           /*nulls=*/false);
  Register(connection.get(), "bq_json_strip_nulls", {kVarchar, kVarchar, kBoolean, kBoolean},
           kVarchar, JsonStripNulls, /*nulls=*/false);
}

}  // namespace bigquery_emulator_duckdb
