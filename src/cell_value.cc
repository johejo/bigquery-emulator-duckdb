#include "src/cell_value.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/common/internal_value.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/interval_value.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/value.h"
#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {
namespace {

using googlesql::Value;
using nlohmann::json;

absl::Status Undecodable(const googlesql::Type* type, std::string_view text) {
  return absl::InternalError(
      absl::StrCat("Cannot decode ", type->DebugString(), " result \"", text, "\""));
}

}  // namespace

absl::StatusOr<googlesql::Value> CellValue(const googlesql::Type* type, const json& cell) {
  if (cell.is_null()) {
    return Value::Null(type);
  }
  if (type->IsArray()) {
    std::vector<Value> elements;
    for (const json& element : cell) {
      GOOGLESQL_ASSIGN_OR_RETURN(elements.emplace_back(),
                                 CellValue(type->AsArray()->element_type(), element.at("v")));
    }
    return googlesql::InternalValue::ArrayNotChecked(
        type->AsArray(), googlesql::InternalValue::kPreservesOrder, std::move(elements));
  }
  if (type->IsStruct()) {
    std::vector<Value> fields;
    const json& cells = cell.at("f");
    for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
      GOOGLESQL_ASSIGN_OR_RETURN(fields.emplace_back(),
                                 CellValue(type->AsStruct()->field(i).type, cells.at(i).at("v")));
    }
    return Value::MakeStruct(type->AsStruct(), std::move(fields));
  }
  const std::string text = cell.get<std::string>();
  switch (type->kind()) {
    case googlesql::TYPE_STRING:
      return Value::String(text);
    case googlesql::TYPE_BYTES: {
      std::string bytes;
      if (!absl::Base64Unescape(text, &bytes)) return Undecodable(type, text);
      return Value::Bytes(bytes);
    }
    case googlesql::TYPE_BOOL:
      if (text != "true" && text != "false") return Undecodable(type, text);
      return Value::Bool(text == "true");
    case googlesql::TYPE_INT64: {
      int64_t number = 0;
      if (!absl::SimpleAtoi(text, &number)) return Undecodable(type, text);
      return Value::Int64(number);
    }
    case googlesql::TYPE_DOUBLE: {
      // BigQuery spells NaN and the infinities NaN, Infinity and -Infinity; clients also accept
      // DuckDB's nan and inf.
      double number = 0;
      if (!absl::SimpleAtod(text, &number)) return Undecodable(type, text);
      return Value::Double(number);
    }
    case googlesql::TYPE_NUMERIC: {
      GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::NumericValue number,
                                 googlesql::NumericValue::FromString(text));
      return Value::Numeric(number);
    }
    case googlesql::TYPE_BIGNUMERIC: {
      GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::BigNumericValue number,
                                 googlesql::BigNumericValue::FromString(text));
      return Value::BigNumeric(number);
    }
    case googlesql::TYPE_DATE: {
      int32_t date = 0;
      GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertStringToDate(text, &date));
      return Value::Date(date);
    }
    case googlesql::TYPE_TIMESTAMP: {
      // Rows carry epoch microseconds until they are rendered for a client.
      int64_t micros = 0;
      if (!absl::SimpleAtoi(text, &micros) ||
          !googlesql::functions::IsValidTimestamp(micros, googlesql::functions::kMicroseconds)) {
        return Undecodable(type, text);
      }
      return Value::TimestampFromUnixMicros(micros);
    }
    case googlesql::TYPE_DATETIME: {
      googlesql::DatetimeValue datetime;
      GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertStringToDatetime(
          text, googlesql::functions::kMicroseconds, &datetime));
      return Value::Datetime(datetime);
    }
    case googlesql::TYPE_TIME: {
      googlesql::TimeValue time;
      GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertStringToTime(
          text, googlesql::functions::kMicroseconds, &time));
      return Value::Time(time);
    }
    case googlesql::TYPE_INTERVAL: {
      GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::IntervalValue interval,
                                 googlesql::IntervalValue::ParseFromString(text, false));
      return Value::Interval(interval);
    }
    case googlesql::TYPE_RANGE: {
      // "[start, end)", whose bounds are cells of the element type or UNBOUNDED.
      const size_t separator = text.find(", ");
      if (text.size() < 6 || text.front() != '[' || text.back() != ')' ||
          separator == std::string::npos) {
        return Undecodable(type, text);
      }
      const googlesql::Type* element = type->AsRange()->element_type();
      std::vector<Value> bounds;
      for (const std::string& bound : {
               text.substr(1, separator - 1),
               text.substr(separator + 2, text.size() - separator - 3),
           }) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            bounds.emplace_back(),
            CellValue(element, bound == "UNBOUNDED" ? json(nullptr) : json(bound)));
      }
      return Value::MakeRange(bounds.at(0), bounds.at(1));
    }
    case googlesql::TYPE_JSON: {
      GOOGLESQL_ASSIGN_OR_RETURN(googlesql::JSONValue value,
                                 googlesql::JSONValue::ParseJSONString(text));
      return Value::Json(std::move(value));
    }
    default:
      return Undecodable(type, text);
  }
}

}  // namespace bigquery_emulator_duckdb
