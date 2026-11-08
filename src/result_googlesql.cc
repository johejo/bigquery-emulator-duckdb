#include "src/result_googlesql.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/time/time.h"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/timestamp_base.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "googlesql/base/status_macros.h"
#include "googlesql/common/internal_value.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/interval_value.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/value.h"
#include "src/bignumeric.h"
#include "src/duckdb_types.h"
#include "src/interval.h"
#include "src/result_chunks.h"
#include "src/result_rows.h"
#include "src/timestamp_nanos.h"
#include "src/vector_value.h"

namespace bigquery_emulator_duckdb {
namespace {

using duckdb::LogicalTypeId;

bool IsInteger(LogicalTypeId id) {
  switch (id) {
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::HUGEINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::UHUGEINT:
      return true;
    default:
      return false;
  }
}

absl::Status Undecodable(const duckdb::LogicalType& type, const googlesql::Type* target) {
  return absl::InternalError("Cannot decode a DuckDB value of type " + type.ToString() + " as " +
                             target->DebugString());
}

// A NULL array is an empty one unless `null_arrays` is set, as in the REST encoding.
absl::StatusOr<googlesql::Value> ToGoogleSqlValue(const duckdb::Vector& vector,
                                                  const duckdb::LogicalType& type, size_t row,
                                                  const googlesql::Type* target, bool null_arrays) {
  if (IsNull(vector, row)) {
    return target->IsArray() && !null_arrays ? googlesql::Value::EmptyArray(target->AsArray())
                                             : googlesql::Value::Null(target);
  }
  const auto id = type.id();
  if (target->IsArray()) {
    if (!IsListLike(type)) {
      return Undecodable(type, target);
    }
    const ListElements list(vector, type, row);
    std::vector<googlesql::Value> elements;
    for (size_t i = 0; i < list.size(); ++i) {
      GOOGLESQL_ASSIGN_OR_RETURN(elements.emplace_back(),
                                 ToGoogleSqlValue(list.child(), list.type(), list.row(i),
                                                  target->AsArray()->element_type(), null_arrays));
    }
    return googlesql::InternalValue::ArrayNotChecked(
        target->AsArray(), googlesql::InternalValue::kPreservesOrder, std::move(elements));
  }
  if (target->IsRange()) {
    if (!IsRange(type)) {
      return Undecodable(type, target);
    }
    std::vector<googlesql::Value> bounds;
    for (size_t i = 0; i < 2; ++i) {
      const auto& child = duckdb::StructVector::GetEntries(vector).at(i);
      const auto& bound = duckdb::StructType::GetChildType(type, i);
      const googlesql::Type* element = target->AsRange()->element_type();
      if (IsFiniteBound(child, bound, row)) {
        GOOGLESQL_ASSIGN_OR_RETURN(bounds.emplace_back(),
                                   ToGoogleSqlValue(child, bound, row, element, null_arrays));
      } else {
        bounds.push_back(googlesql::Value::Null(element));
      }
    }
    return googlesql::Value::MakeRange(bounds.at(0), bounds.at(1));
  }
  if (target->IsStruct()) {
    const googlesql::StructType* fields = target->AsStruct();
    // DuckDB holds a STRUCT<> as a STRUCT with a placeholder field.
    if (id != LogicalTypeId::STRUCT ||
        (fields->num_fields() != 0 &&
         duckdb::StructType::GetChildCount(type) != static_cast<size_t>(fields->num_fields()))) {
      return Undecodable(type, target);
    }
    std::vector<googlesql::Value> values;
    for (int i = 0; i < fields->num_fields(); ++i) {
      GOOGLESQL_ASSIGN_OR_RETURN(values.emplace_back(),
                                 ToGoogleSqlValue(duckdb::StructVector::GetEntries(vector).at(i),
                                                  duckdb::StructType::GetChildType(type, i), row,
                                                  fields->field(i).type, null_arrays));
    }
    return googlesql::Value::MakeStruct(fields, std::move(values));
  }
  switch (target->kind()) {
    case googlesql::TYPE_STRING:
      if (id == LogicalTypeId::VARCHAR) {
        return googlesql::Value::String(VectorString(vector, row));
      }
      break;
    case googlesql::TYPE_BYTES:
      if (id == LogicalTypeId::BLOB) {
        return googlesql::Value::Bytes(VectorString(vector, row));
      }
      break;
    case googlesql::TYPE_BOOL:
      if (id == LogicalTypeId::BOOLEAN) {
        return googlesql::Value::Bool(VectorElement<bool>(vector, row));
      }
      break;
    case googlesql::TYPE_INT64: {
      if (id == LogicalTypeId::BIGINT) {
        return googlesql::Value::Int64(VectorElement<int64_t>(vector, row));
      }
      int64_t number = 0;
      if (IsInteger(id) && absl::SimpleAtoi(ValueString(VectorValue(vector, type, row)), &number)) {
        return googlesql::Value::Int64(number);
      }
      break;
    }
    case googlesql::TYPE_DOUBLE:
      if (id == LogicalTypeId::DOUBLE) {
        return googlesql::Value::Double(VectorElement<double>(vector, row));
      }
      if (id == LogicalTypeId::FLOAT) {
        return googlesql::Value::Double(VectorElement<float>(vector, row));
      }
      break;
    case googlesql::TYPE_NUMERIC:
      if (id == LogicalTypeId::DECIMAL || IsInteger(id)) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            const googlesql::NumericValue number,
            googlesql::NumericValue::FromString(ValueString(VectorValue(vector, type, row))));
        return googlesql::Value::Numeric(number);
      }
      break;
    case googlesql::TYPE_BIGNUMERIC: {
      if (id == LogicalTypeId::BIGNUM) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            const googlesql::BigNumericValue number,
            BigNumericFromUnits(ValueString(VectorValue(vector, type, row))));
        return googlesql::Value::BigNumeric(number);
      }
      if (id == LogicalTypeId::DECIMAL || IsInteger(id)) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            const googlesql::BigNumericValue number,
            googlesql::BigNumericValue::FromString(ValueString(VectorValue(vector, type, row))));
        return googlesql::Value::BigNumeric(number);
      }
      break;
    }
    case googlesql::TYPE_DATE:
      if (id == LogicalTypeId::DATE) {
        const int32_t days = VectorElement<duckdb::date_t>(vector, row).days;
        if (googlesql::functions::IsValidDate(days)) {
          return googlesql::Value::Date(days);
        }
      }
      break;
    case googlesql::TYPE_TIMESTAMP:
      if (id == LogicalTypeId::HUGEINT) {
        const auto nanos = VectorElement<duckdb::hugeint_t>(vector, row);
        GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time time,
                                   TimestampFromNanos(TimestampNanosInteger(
                                       {.lower = nanos.lower, .upper = nanos.upper})));
        return googlesql::Value::Timestamp(time);
      }
      if (id == LogicalTypeId::TIMESTAMP_TZ) {
        const int64_t micros = VectorElement<duckdb::timestamp_t>(vector, row).value;
        if (googlesql::functions::IsValidTimestamp(micros, googlesql::functions::kMicroseconds)) {
          return googlesql::Value::TimestampFromUnixMicros(micros);
        }
      }
      break;
    case googlesql::TYPE_DATETIME:
      if (id == LogicalTypeId::TIMESTAMP) {
        const auto timestamp = VectorElement<duckdb::timestamp_t>(vector, row);
        if (!timestamp.IsFinite()) {
          break;
        }
        int32_t year = 0;
        int32_t month = 0;
        int32_t day = 0;
        int32_t hour = 0;
        int32_t minute = 0;
        int32_t second = 0;
        int32_t micros = 0;
        duckdb::Date::Convert(duckdb::Timestamp::GetDate(timestamp), year, month, day);
        duckdb::Time::Convert(duckdb::Timestamp::GetTime(timestamp), hour, minute, second, micros);
        const auto datetime = googlesql::DatetimeValue::FromYMDHMSAndMicros(year, month, day, hour,
                                                                            minute, second, micros);
        if (datetime.IsValid()) {
          return googlesql::Value::Datetime(datetime);
        }
      }
      break;
    case googlesql::TYPE_TIME:
      if (id == LogicalTypeId::TIME) {
        int32_t hour = 0;
        int32_t minute = 0;
        int32_t second = 0;
        int32_t micros = 0;
        duckdb::Time::Convert(VectorElement<duckdb::dtime_t>(vector, row), hour, minute, second,
                              micros);
        const auto time = googlesql::TimeValue::FromHMSAndMicros(hour, minute, second, micros);
        if (time.IsValid()) {
          return googlesql::Value::Time(time);
        }
      }
      break;
    case googlesql::TYPE_INTERVAL:
      if (id == LogicalTypeId::INTERVAL) {
        const auto stored = VectorElement<duckdb::interval_t>(vector, row);
        GOOGLESQL_ASSIGN_OR_RETURN(
            const googlesql::IntervalValue interval,
            IntervalFromDuckDb(
                {.months = stored.months, .days = stored.days, .micros = stored.micros}));
        return googlesql::Value::Interval(interval);
      }
      break;
    case googlesql::TYPE_JSON:
      if (id == LogicalTypeId::VARCHAR) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            googlesql::JSONValue value,
            googlesql::JSONValue::ParseJSONString(VectorString(vector, row)));
        return googlesql::Value::Json(std::move(value));
      }
      break;
    default:
      break;
  }
  return Undecodable(type, target);
}

}  // namespace

absl::StatusOr<googlesql::Value> ReadGoogleSqlValue(const ResultRows& rows, size_t row,
                                                    size_t column, const googlesql::Type* type) {
  const ResultChunks* chunks = rows.chunks();
  if (chunks == nullptr || row >= chunks->row_count() || column >= chunks->column_count()) {
    throw std::out_of_range("Query result value is out of range");
  }
  absl::StatusOr<googlesql::Value> value;
  chunks->Runs(row, 1, [&](const duckdb::DataChunk& chunk, size_t index, size_t, size_t) {
    value = ToGoogleSqlValue(chunk.data.at(column), chunks->type(column), index, type,
                             rows.null_arrays());
  });
  return value;
}

}  // namespace bigquery_emulator_duckdb
