#include "src/backend_functions/datetime.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/functions/datetime.pb.h"
#include "googlesql/public/functions/generate_array.h"
#include "googlesql/public/functions/parse_date_time.h"
#include "googlesql/public/functions/string_format.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "src/backend_functions/apply.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/bignumeric.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::backend_functions {

googlesql::TimeValue TimeFromMicros(int64_t micros) {
  constexpr int64_t kPerSecond = 1000000;
  const int64_t seconds = micros / kPerSecond;
  return googlesql::TimeValue::FromHMSAndMicros(
      static_cast<int32_t>(seconds / 3600), static_cast<int32_t>(seconds / 60 % 60),
      static_cast<int32_t>(seconds % 60), static_cast<int32_t>(micros % kPerSecond));
}

int64_t MicrosFromTime(const googlesql::TimeValue& time) {
  return (((((int64_t{time.Hour()} * 60) + time.Minute()) * 60) + time.Second()) * 1000000) +
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

namespace {

// The DuckDB type of each argument column, and whether it is JSON, a VARCHAR by its alias.
struct ColumnType {
  duckdb_type id;
  bool json;
};

std::vector<ColumnType> ColumnTypes(duckdb_data_chunk input) {
  std::vector<ColumnType> types;
  for (idx_t column = 0; column < duckdb_data_chunk_get_column_count(input); ++column) {
    LogicalType const type(
        duckdb_vector_get_column_type(duckdb_data_chunk_get_vector(input, column)));
    const DuckString alias(duckdb_logical_type_get_alias(type.get()));
    types.push_back({
        .id = duckdb_get_type_id(type.get()),
        .json = alias != nullptr && std::string_view(alias.get()) == "JSON",
    });
  }
  return types;
}

namespace fn = googlesql::functions;

// BigQuery expands %Q to the quarter and %J to the ISO day of the year.
constexpr fn::FormatDateTimestampOptions kFormatOptions = {
    .expand_Q = true,
    .expand_J = true,
};

// Expands %G, the ISO 8601 year, in format for day. GoogleSQL leaves it to the C library's
// strftime(), and macOS pads years below 1000 with zeros where BigQuery, like glibc, does not.
std::string ExpandIsoYear(std::string_view format, absl::CivilDay day) {
  // The ISO year is the year of the week's Thursday; absl::Weekday counts from Monday.
  const absl::CivilDay thursday = day + (3 - static_cast<int>(absl::GetWeekday(day)));
  std::string expanded;
  for (size_t pct = format.find('%'); pct != std::string_view::npos && pct + 1 < format.size();
       pct = format.find('%')) {
    expanded.append(format.substr(0, pct));
    const std::string_view element = format.substr(pct, 2);
    expanded += element == "%G" ? std::to_string(thursday.year()) : std::string(element);
    format.remove_prefix(pct + 2);
  }
  expanded.append(format);
  return expanded;
}

// FORMAT_DATE(format, date).
absl::Status FormatDate(std::string_view format, int32_t date, std::string* out) {
  if (format.find("%G") != std::string_view::npos) {
    return fn::FormatDateToString(ExpandIsoYear(format, absl::CivilDay(1970, 1, 1) + date), date,
                                  kFormatOptions, out);
  }
  return fn::FormatDateToString(format, date, kFormatOptions, out);
}

// FORMAT_DATETIME(format, datetime).
absl::Status FormatDatetime(std::string_view format, int64_t micros, std::string* out) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(micros));
  if (format.find("%G") != std::string_view::npos) {
    return fn::FormatDatetimeToStringWithOptions(
        ExpandIsoYear(format, absl::ToCivilDay(absl::FromUnixMicros(micros), absl::UTCTimeZone())),
        datetime, kFormatOptions, out);
  }
  return fn::FormatDatetimeToStringWithOptions(format, datetime, kFormatOptions, out);
}

// FORMAT_TIME(format, time).
absl::Status FormatTime(std::string_view format, int64_t micros, std::string* out) {
  return fn::FormatTimeToString(format, TimeFromMicros(micros), out);
}

// FORMAT_TIMESTAMP(format, timestamp, time_zone).
absl::Status FormatTimestamp(std::string_view format, int64_t micros, std::string_view time_zone,
                             std::string* out) {
  absl::TimeZone zone;
  if (format.find("%G") != std::string_view::npos && fn::MakeTimeZone(time_zone, &zone).ok()) {
    return fn::FormatTimestampToString(
        ExpandIsoYear(format, absl::ToCivilDay(absl::FromUnixMicros(micros), zone)), micros,
        time_zone, kFormatOptions, out);
  }
  return fn::FormatTimestampToString(format, micros, time_zone, kFormatOptions, out);
}

// PARSE_DATE(format, string).
absl::Status ParseDate(std::string_view format, std::string_view text, int32_t* out) {
  return fn::ParseStringToDate(format, text, /*parse_version2=*/true, out);
}

// PARSE_DATETIME(format, string).
absl::StatusOr<int64_t> ParseDatetime(std::string_view format, std::string_view text) {
  googlesql::DatetimeValue out;
  GOOGLESQL_RETURN_IF_ERROR(fn::ParseStringToDatetime(format, text, fn::kMicroseconds,
                                                      /*parse_version2=*/true, &out));
  return MicrosFromDatetime(out);
}

// PARSE_TIME(format, string).
absl::StatusOr<int64_t> ParseTime(std::string_view format, std::string_view text) {
  googlesql::TimeValue out;
  GOOGLESQL_RETURN_IF_ERROR(fn::ParseStringToTime(format, text, fn::kMicroseconds, &out));
  return MicrosFromTime(out);
}

// PARSE_TIMESTAMP(format, string, time_zone), with the time zone for a string without one.
absl::Status ParseTimestamp(std::string_view format, std::string_view text,
                            std::string_view time_zone, int64_t* out) {
  return fn::ParseStringToTimestamp(format, text, time_zone, /*parse_version2=*/true, out);
}

// CAST(string AS DATE), which accepts only BigQuery's canonical format, where DuckDB accepts more.
absl::Status StringToDate(std::string_view text, int32_t* out) {
  return fn::ConvertStringToDate(text, out);
}

// CAST(string AS DATETIME).
absl::StatusOr<int64_t> StringToDatetime(std::string_view text) {
  googlesql::DatetimeValue out;
  GOOGLESQL_RETURN_IF_ERROR(fn::ConvertStringToDatetime(text, fn::kMicroseconds, &out));
  return MicrosFromDatetime(out);
}

// CAST(string AS TIME).
absl::StatusOr<int64_t> StringToTime(std::string_view text) {
  googlesql::TimeValue out;
  GOOGLESQL_RETURN_IF_ERROR(fn::ConvertStringToTime(text, fn::kMicroseconds, &out));
  return MicrosFromTime(out);
}

// CAST(string AS TIMESTAMP), in UTC for a string without a time zone.
absl::Status StringToTimestamp(std::string_view text, int64_t* out) {
  return fn::ConvertStringToTimestamp(text, absl::UTCTimeZone(), fn::kMicroseconds,
                                      /*allow_tz_in_str=*/true, out);
}

// TIMESTAMP(string, time_zone), whose string may not have a time zone of its own.
absl::Status StringToTimestampIn(std::string_view text, std::string_view time_zone, int64_t* out) {
  return fn::ConvertStringToTimestamp(text, time_zone, fn::kMicroseconds,
                                      /*allow_tz_in_str=*/false, out);
}

// TIMESTAMP_TRUNC, DATE_TRUNC and DATETIME_TRUNC of a TIMESTAMP to a date part in a time zone,
// such as 'day' or 'week_monday'.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the date part and the time zone.
absl::Status TimestampTrunc(int64_t micros, std::string_view part, std::string_view time_zone,
                            int64_t* out) {
  fn::DateTimestampPart value = fn::DATE;
  if (!fn::DateTimestampPart_Parse(absl::AsciiStrToUpper(part), &value)) {
    return absl::InvalidArgumentError("Unsupported date part");
  }
  return fn::TimestampTrunc(micros, time_zone, value, out);
}

// GENERATE_DATE_ARRAY(start, end, INTERVAL step part), with BigQuery's documented
// 1,048,576-element limit. Like BigQuery, each step adds to the previous element, so an element
// clamped to the end of a shorter month moves the rest to that day.
absl::Status GenerateDateArray(int32_t start, int32_t end, int64_t step, std::string_view part,
                               std::vector<int32_t>* out) {
  constexpr int kMaxGeneratedArraySize = 1048576;
  fn::DateTimestampPart unit = fn::DAY;
  if (!fn::DateTimestampPart_Parse(absl::AsciiStrToUpper(part), &unit)) {
    return absl::InvalidArgumentError("Unsupported date part");
  }
  std::vector<int64_t> dates;
  GOOGLESQL_RETURN_IF_ERROR((fn::GenerateArrayHelper<fn::ArrayGenTrait<int64_t, fn::DateIncrement>,
                                                     kMaxGeneratedArraySize>(
      start, end, {.unit = unit, .value = step}, &dates)));
  out->assign(dates.begin(), dates.end());
  return absl::OkStatus();
}

// CAST(datetime AS STRING), whose fractional seconds have three or six digits, where DuckDB
// leaves out trailing zeros.
absl::Status DatetimeToString(int64_t micros, std::string* out) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(micros));
  return fn::ConvertDatetimeToString(datetime, fn::kMicroseconds, out);
}

// CAST(time AS STRING).
absl::Status TimeToString(int64_t micros, std::string* out) {
  return fn::ConvertTimeToString(TimeFromMicros(micros), fn::kMicroseconds, out);
}

// CAST(timestamp AS STRING) and STRING(timestamp, time_zone), in the given time zone.
absl::Status TimestampToString(int64_t micros, std::string_view time_zone, std::string* out) {
  return fn::ConvertTimestampToString(absl::FromUnixMicros(micros), fn::kMicroseconds, time_zone,
                                      out);
}

// A RANGE argument to FORMAT, the only STRUCT the handler passes, whose unbounded ends are
// infinite in DuckDB and NULL in GoogleSQL.
absl::StatusOr<googlesql::Value> FormatRange(duckdb_vector vector, idx_t row, bool null) {
  LogicalType const type(duckdb_vector_get_column_type(vector));
  LogicalType const element(duckdb_struct_type_child_type(type.get(), 0));
  const duckdb_type element_type = duckdb_get_type_id(element.get());
  const googlesql::Type* range = element_type == kDate ? googlesql::types::DateRangeType()
                                 : element_type == kDatetime
                                     ? googlesql::types::DatetimeRangeType()
                                     : googlesql::types::TimestampRangeType();
  if (null) {
    return googlesql::Value::Null(range);
  }
  std::vector<googlesql::Value> bounds;
  for (idx_t i = 0; i < 2; ++i) {
    duckdb_vector child = duckdb_struct_vector_get_child(vector, i);
    if (element_type == kDate) {
      const auto days = VectorElement<int32_t>(child, row);
      bounds.push_back(duckdb_is_finite_date({days})
                           ? googlesql::Value::Date(days)
                           : googlesql::Value::Null(range->AsRange()->element_type()));
      continue;
    }
    const auto micros = VectorElement<int64_t>(child, row);
    if (!duckdb_is_finite_timestamp({micros})) {
      bounds.push_back(googlesql::Value::Null(range->AsRange()->element_type()));
    } else if (element_type == kDatetime) {
      GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(micros));
      bounds.push_back(googlesql::Value::Datetime(datetime));
    } else {
      bounds.push_back(googlesql::Value::Timestamp(absl::FromUnixMicros(micros)));
    }
  }
  return googlesql::Value::MakeRange(bounds.at(0), bounds.at(1));
}

// A FORMAT argument as the GoogleSQL value of the type its DuckDB type stands for.
absl::StatusOr<googlesql::Value> FormatArgument(const Arguments& arguments, idx_t column,
                                                ColumnType type) {
  const bool null = arguments.IsNull(column);
  if (type.json) {
    // Parsed as the JSON functions parse it.
    if (null) {
      return googlesql::Value::Null(googlesql::types::JsonType());
    }
    GOOGLESQL_ASSIGN_OR_RETURN(auto json,
                               googlesql::JSONValue::ParseJSONString(arguments.String(column)));
    return googlesql::Value::Json(std::move(json));
  }
  switch (type.id) {
    case DUCKDB_TYPE_VARCHAR:
      return null ? googlesql::Value::NullString()
                  : googlesql::Value::String(arguments.String(column));
    case DUCKDB_TYPE_BLOB:
      return null ? googlesql::Value::NullBytes()
                  : googlesql::Value::Bytes(arguments.String(column));
    case DUCKDB_TYPE_BIGINT:
      return null ? googlesql::Value::NullInt64() : googlesql::Value::Int64(arguments.Int(column));
    case DUCKDB_TYPE_DOUBLE: {
      if (null) {
        return googlesql::Value::NullDouble();
      }
      // GoogleSQL prints the sign of a negative NaN when it preserves the sign of zero; BigQuery
      // does not.
      const double value = arguments.Double(column);
      return googlesql::Value::Double(std::isnan(value) ? std::numeric_limits<double>::quiet_NaN()
                                                        : value);
    }
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
    case DUCKDB_TYPE_BIGNUM: {
      if (null) {
        return googlesql::Value::NullBigNumeric();
      }
      const auto bignumeric = BigNumericFromBignum(arguments.String(column));
      if (!bignumeric.ok()) {
        return bignumeric.status();
      }
      return googlesql::Value::BigNumeric(*bignumeric);
    }
    case DUCKDB_TYPE_DECIMAL: {
      if (null) {
        return googlesql::Value::NullNumeric();
      }
      const auto numeric = googlesql::NumericValue::FromPackedInt(arguments.Decimal(column));
      if (!numeric.ok()) {
        return numeric.status();
      }
      return googlesql::Value::Numeric(*numeric);
    }
    case DUCKDB_TYPE_STRUCT:
      return FormatRange(arguments.Vector(column), arguments.Row(), null);
    default:
      return absl::UnimplementedError("Unsupported argument type for FORMAT");
  }
}

// A FORMAT pattern and its values, rewritten by AdaptFormat.
struct AdaptedFormat {
  std::string pattern;
  std::vector<googlesql::Value> values;
};

// Rewrites the specifiers whose output from GoogleSQL differs from BigQuery's, and their values,
// so that GoogleSQL prints BigQuery's result; nullopt when nothing changes. The scan follows
// GoogleSQL's "%[flags][width][.precision]specifier" and assigns arguments as it does. A
// specifier GoogleSQL rejects, or whose arguments are missing, is copied as is so that GoogleSQL
// reports it.
std::optional<AdaptedFormat> AdaptFormat(std::string_view pattern,
                                         const std::vector<googlesql::Value>& values) {
  // GoogleSQL's limit on the output width, which also caps an integer's precision.
  constexpr int64_t kMaxPrecision = 1048576;
  constexpr std::string_view kDigits = "0123456789";
  AdaptedFormat adapted{.pattern = "", .values = values};
  bool changed = false;
  size_t next_arg = 0;
  size_t position = 0;
  while (position < pattern.size()) {
    const size_t percent = pattern.find('%', position);
    if (percent == std::string_view::npos) {
      adapted.pattern.append(pattern.substr(position));
      break;
    }
    adapted.pattern.append(pattern.substr(position, percent - position));
    size_t cursor = percent + 1;
    const auto span = [&](std::string_view chars) {
      const size_t begin = cursor;
      while (cursor < pattern.size() && chars.find(pattern.at(cursor)) != std::string_view::npos) {
        ++cursor;
      }
      return std::string(pattern.substr(begin, cursor - begin));
    };
    const auto star = [&] {
      const bool found = cursor < pattern.size() && pattern.at(cursor) == '*';
      cursor += found ? 1 : 0;
      return found;
    };
    std::string flags = span("-+ #'0");
    const std::string width = star() ? "*" : span(kDigits);
    std::optional<std::string> precision;
    if (cursor < pattern.size() && pattern.at(cursor) == '.') {
      ++cursor;
      precision = star() ? "*" : span(kDigits);
    }
    if (cursor == pattern.size() || pattern.at(cursor) == '\n') {
      adapted.pattern.append(pattern.substr(percent));
      break;
    }
    char conversion = pattern.at(cursor++);
    position = cursor;
    std::optional<size_t> precision_arg;
    if (conversion != '%') {
      next_arg += width == "*" ? 1 : 0;
      if (precision == "*") {
        precision_arg = next_arg++;
      }
    }
    const size_t value_arg = next_arg++;
    if (conversion == '%' || value_arg >= values.size() || values.at(value_arg).is_null()) {
      adapted.pattern.append(pattern.substr(percent, cursor - percent));
      continue;
    }
    const googlesql::Value& value = values.at(value_arg);
    if (conversion == 't' && value.type_kind() == googlesql::TYPE_STRING &&
        value.string_value().find('\0') != std::string::npos) {
      // BigQuery truncates a STRING at NUL for %t, as GoogleSQL does for %s.
      const std::string& text = value.string_value();
      adapted.values.at(value_arg) = googlesql::Value::String(text.substr(0, text.find('\0')));
      changed = true;
    } else if (std::string_view("dioxX").find(conversion) != std::string_view::npos &&
               value.type_kind() == googlesql::TYPE_INT64) {
      // The explicit precision, or nullopt when it is absent, negative, NULL or above the limit.
      std::optional<int64_t> digits;
      if (precision_arg.has_value()) {
        const googlesql::Value& argument = values.at(*precision_arg);
        if (argument.type_kind() == googlesql::TYPE_INT64 && !argument.is_null() &&
            argument.int64_value() >= 0 && argument.int64_value() <= kMaxPrecision) {
          digits = argument.int64_value();
        }
      } else if (precision.has_value()) {
        int64_t parsed = 0;
        const auto [end, error] =
            std::from_chars(precision->data(), precision->data() + precision->size(), parsed);
        if (error == std::errc() && parsed <= kMaxPrecision) {
          digits = parsed;
        }
      }
      if (digits.has_value()) {
        // An explicit precision disables zero padding.
        std::erase(flags, '0');
        const bool grouping = flags.find('\'') != std::string::npos;
        if (grouping && value.int64_value() == 0 && *digits == 0) {
          // A grouped zero at precision zero prints only its sign, without digits or prefix.
          const std::string sign = flags.find('+') != std::string::npos   ? "+"
                                   : flags.find(' ') != std::string::npos ? " "
                                                                          : "";
          flags = flags.find('-') != std::string::npos ? "-" : "";
          conversion = 's';
          adapted.values.at(value_arg) = googlesql::Value::String(sign);
          if (precision_arg.has_value()) {
            adapted.values.at(*precision_arg) = googlesql::Value::Int64(1);
          } else {
            precision.reset();
          }
          changed = true;
        } else if (grouping) {
          // BigQuery counts grouping separators toward the precision; each group after the
          // first takes one more character than it has digits.
          const int64_t group = conversion == 'd' || conversion == 'i' ? 3 : 4;
          const int64_t counted = *digits - ((*digits - 1) / (group + 1));
          if (precision_arg.has_value()) {
            adapted.values.at(*precision_arg) = googlesql::Value::Int64(counted);
            changed = true;
          } else {
            precision = std::to_string(counted);
          }
        }
      }
    }
    adapted.pattern.append("%").append(flags).append(width);
    if (precision.has_value()) {
      adapted.pattern.append(".").append(*precision);
    }
    adapted.pattern.push_back(conversion);
  }
  if (!changed && adapted.pattern == pattern) {
    return std::nullopt;
  }
  return adapted;
}

// FORMAT(format, values...), which is NULL for a NULL format and where GoogleSQL says so for
// a NULL value.
void Format(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const std::vector<ColumnType> types = ColumnTypes(input);
  EachRow(
      info, input, output,
      [&types](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        std::vector<googlesql::Value> values;
        for (idx_t column = 1; column < types.size(); ++column) {
          auto value = FormatArgument(arguments, column, types.at(column));
          if (!value.ok()) {
            return value.status();
          }
          values.push_back(*std::move(value));
        }
        const std::string pattern = arguments.String(0);
        const std::optional<AdaptedFormat> adapted = AdaptFormat(pattern, values);
        std::string out;
        bool is_null = false;
        const auto format = [&](const std::string& format_pattern,
                                const std::vector<googlesql::Value>& format_values) {
          // BigQuery preserves the sign of floating-point zero in FORMAT.
          return googlesql::functions::StringFormatUtf8(
              format_pattern, format_values, googlesql::PRODUCT_EXTERNAL, &out, &is_null,
              /*canonicalize_zero=*/false, /*use_external_float32=*/true);
        };
        absl::Status status = adapted.has_value() ? format(adapted->pattern, adapted->values)
                                                  : format(pattern, values);
        if (!status.ok() && adapted.has_value()) {
          // Report an error as for the format the query gave, whose text the message quotes.
          const absl::Status original = format(pattern, values);
          if (!original.ok()) {
            status = original;
          }
        }
        if (!status.ok()) {
          return status;
        }
        return is_null ? std::nullopt : std::optional<std::string>(out);
      },
      /*nulls=*/false);
}

}  // namespace

void RegisterDatetimeFunctions(duckdb_connection connection) {
  Register(connection, "bq_format_date", {kVarchar, kDate}, kVarchar, Apply<FormatDate>);
  Register(connection, "bq_format_datetime", {kVarchar, kDatetime}, kVarchar,
           Apply<FormatDatetime>);
  Register(connection, "bq_format_time", {kVarchar, kTime}, kVarchar, Apply<FormatTime>);
  Register(connection, "bq_format_timestamp", {kVarchar, kTimestamp, kVarchar}, kVarchar,
           Apply<FormatTimestamp>);
  Register(connection, "bq_parse_date", {kVarchar, kVarchar}, kDate, Apply<ParseDate>);
  Register(connection, "bq_parse_datetime", {kVarchar, kVarchar}, kDatetime, Apply<ParseDatetime>);
  Register(connection, "bq_parse_time", {kVarchar, kVarchar}, kTime, Apply<ParseTime>);
  Register(connection, "bq_parse_timestamp", {kVarchar, kVarchar, kVarchar}, kTimestamp,
           Apply<ParseTimestamp>);
  Register(connection, "bq_string_to_date", {kVarchar, kBoolean}, kDate,
           Apply<StringToDate, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_string_to_datetime", {kVarchar, kBoolean}, kDatetime,
           Apply<StringToDatetime, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_string_to_time", {kVarchar, kBoolean}, kTime,
           Apply<StringToTime, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_string_to_timestamp", {kVarchar, kBoolean}, kTimestamp,
           Apply<StringToTimestamp, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_string_to_timestamp_in", {kVarchar, kVarchar}, kTimestamp,
           Apply<StringToTimestampIn>);
  Register(connection, "bq_timestamp_trunc", {kTimestamp, kVarchar, kVarchar}, kTimestamp,
           Apply<TimestampTrunc>);
  LogicalType const date(duckdb_create_logical_type(kDate));
  LogicalType const dates(duckdb_create_list_type(date.get()));
  Register(connection, "bq_generate_date_array", {kDate, kDate, kBigint, kVarchar}, dates.get(),
           Apply<GenerateDateArray>);
  Register(connection, "bq_datetime_string", {kDatetime}, kVarchar, Apply<DatetimeToString>);
  Register(connection, "bq_time_string", {kTime}, kVarchar, Apply<TimeToString>);
  Register(connection, "bq_timestamp_string", {kTimestamp, kVarchar}, kVarchar,
           Apply<TimestampToString>);
  Register(connection, "bq_format", {kVarchar}, kVarchar, Format, /*nulls=*/false, DUCKDB_TYPE_ANY);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
