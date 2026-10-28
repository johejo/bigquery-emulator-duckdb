#include "src/result_googlesql.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "duckdb.h"
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
#include "src/duckdb_handle.h"
#include "src/duckdb_types.h"
#include "src/interval.h"
#include "src/result_chunks.h"
#include "src/result_rows.h"
#include "src/vector_value.h"

namespace bigquery_emulator_duckdb {
namespace {

bool IsInteger(duckdb_type id) {
  switch (id) {
    case DUCKDB_TYPE_TINYINT:
    case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_INTEGER:
    case DUCKDB_TYPE_BIGINT:
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UTINYINT:
    case DUCKDB_TYPE_USMALLINT:
    case DUCKDB_TYPE_UINTEGER:
    case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_UHUGEINT:
      return true;
    default:
      return false;
  }
}

absl::Status Undecodable(duckdb_logical_type type, const googlesql::Type* target) {
  return absl::InternalError("Cannot decode a DuckDB value of type " +
                             std::to_string(duckdb_get_type_id(type)) + " as " +
                             target->DebugString());
}

// A NULL array is an empty one unless `null_arrays` is set, as in the REST encoding.
absl::StatusOr<googlesql::Value> ToGoogleSqlValue(duckdb_vector vector, duckdb_logical_type type,
                                                  idx_t row, const googlesql::Type* target,
                                                  bool null_arrays) {
  if (IsNull(vector, row)) {
    return target->IsArray() && !null_arrays ? googlesql::Value::EmptyArray(target->AsArray())
                                             : googlesql::Value::Null(target);
  }
  const duckdb_type id = duckdb_get_type_id(type);
  if (target->IsArray()) {
    if (!IsListLike(type)) {
      return Undecodable(type, target);
    }
    const ListElements list(vector, type, row);
    std::vector<googlesql::Value> elements;
    for (idx_t i = 0; i < list.size(); ++i) {
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
    for (idx_t i = 0; i < 2; ++i) {
      duckdb_vector child = duckdb_struct_vector_get_child(vector, i);
      LogicalType const bound(duckdb_struct_type_child_type(type, i));
      const googlesql::Type* element = target->AsRange()->element_type();
      if (IsFiniteBound(child, bound.get(), row)) {
        GOOGLESQL_ASSIGN_OR_RETURN(bounds.emplace_back(),
                                   ToGoogleSqlValue(child, bound.get(), row, element, null_arrays));
      } else {
        bounds.push_back(googlesql::Value::Null(element));
      }
    }
    return googlesql::Value::MakeRange(bounds.at(0), bounds.at(1));
  }
  if (target->IsStruct()) {
    const googlesql::StructType* fields = target->AsStruct();
    // DuckDB holds a STRUCT<> as a STRUCT with a placeholder field.
    if (id != DUCKDB_TYPE_STRUCT ||
        (fields->num_fields() != 0 &&
         duckdb_struct_type_child_count(type) != static_cast<idx_t>(fields->num_fields()))) {
      return Undecodable(type, target);
    }
    std::vector<googlesql::Value> values;
    for (int i = 0; i < fields->num_fields(); ++i) {
      LogicalType const child_type(duckdb_struct_type_child_type(type, i));
      GOOGLESQL_ASSIGN_OR_RETURN(
          values.emplace_back(),
          ToGoogleSqlValue(duckdb_struct_vector_get_child(vector, i), child_type.get(), row,
                           fields->field(i).type, null_arrays));
    }
    return googlesql::Value::MakeStruct(fields, std::move(values));
  }
  switch (target->kind()) {
    case googlesql::TYPE_STRING:
      if (id == DUCKDB_TYPE_VARCHAR) {
        return googlesql::Value::String(VectorString(vector, row));
      }
      break;
    case googlesql::TYPE_BYTES:
      if (id == DUCKDB_TYPE_BLOB) {
        return googlesql::Value::Bytes(VectorString(vector, row));
      }
      break;
    case googlesql::TYPE_BOOL:
      if (id == DUCKDB_TYPE_BOOLEAN) {
        return googlesql::Value::Bool(VectorElement<bool>(vector, row));
      }
      break;
    case googlesql::TYPE_INT64: {
      if (id == DUCKDB_TYPE_BIGINT) {
        return googlesql::Value::Int64(VectorElement<int64_t>(vector, row));
      }
      // Other integer types, such as a HUGEINT sum, hold an INT64 only within its range.
      int64_t number = 0;
      if (IsInteger(id) &&
          absl::SimpleAtoi(ValueString(VectorValue(vector, type, row).get()), &number)) {
        return googlesql::Value::Int64(number);
      }
      break;
    }
    case googlesql::TYPE_DOUBLE:
      if (id == DUCKDB_TYPE_DOUBLE) {
        return googlesql::Value::Double(VectorElement<double>(vector, row));
      }
      if (id == DUCKDB_TYPE_FLOAT) {
        return googlesql::Value::Double(VectorElement<float>(vector, row));
      }
      break;
    case googlesql::TYPE_NUMERIC:
      if (id == DUCKDB_TYPE_DECIMAL || IsInteger(id)) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            const googlesql::NumericValue number,
            googlesql::NumericValue::FromString(ValueString(VectorValue(vector, type, row).get())));
        return googlesql::Value::Numeric(number);
      }
      break;
    case googlesql::TYPE_BIGNUMERIC: {
      if (id == DUCKDB_TYPE_BIGNUM) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            const googlesql::BigNumericValue number,
            BigNumericFromUnits(ValueString(VectorValue(vector, type, row).get())));
        return googlesql::Value::BigNumeric(number);
      }
      if (id == DUCKDB_TYPE_DECIMAL || IsInteger(id)) {
        GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::BigNumericValue number,
                                   googlesql::BigNumericValue::FromString(
                                       ValueString(VectorValue(vector, type, row).get())));
        return googlesql::Value::BigNumeric(number);
      }
      break;
    }
    case googlesql::TYPE_DATE:
      if (id == DUCKDB_TYPE_DATE) {
        const int32_t days = VectorElement<duckdb_date>(vector, row).days;
        if (googlesql::functions::IsValidDate(days)) {
          return googlesql::Value::Date(days);
        }
      }
      break;
    case googlesql::TYPE_TIMESTAMP:
      if (id == DUCKDB_TYPE_TIMESTAMP_TZ) {
        const int64_t micros = VectorElement<duckdb_timestamp>(vector, row).micros;
        if (googlesql::functions::IsValidTimestamp(micros, googlesql::functions::kMicroseconds)) {
          return googlesql::Value::TimestampFromUnixMicros(micros);
        }
      }
      break;
    case googlesql::TYPE_DATETIME:
      if (id == DUCKDB_TYPE_TIMESTAMP) {
        const auto timestamp = VectorElement<duckdb_timestamp>(vector, row);
        if (!duckdb_is_finite_timestamp(timestamp)) {
          break;
        }
        const duckdb_timestamp_struct parts = duckdb_from_timestamp(timestamp);
        const auto datetime = googlesql::DatetimeValue::FromYMDHMSAndMicros(
            parts.date.year, parts.date.month, parts.date.day, parts.time.hour, parts.time.min,
            parts.time.sec, parts.time.micros);
        if (datetime.IsValid()) {
          return googlesql::Value::Datetime(datetime);
        }
      }
      break;
    case googlesql::TYPE_TIME:
      if (id == DUCKDB_TYPE_TIME) {
        const duckdb_time_struct parts = duckdb_from_time(VectorElement<duckdb_time>(vector, row));
        const auto time =
            googlesql::TimeValue::FromHMSAndMicros(parts.hour, parts.min, parts.sec, parts.micros);
        if (time.IsValid()) {
          return googlesql::Value::Time(time);
        }
      }
      break;
    case googlesql::TYPE_INTERVAL:
      if (id == DUCKDB_TYPE_INTERVAL) {
        GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::IntervalValue interval,
                                   IntervalFromDuckDb(VectorElement<duckdb_interval>(vector, row)));
        return googlesql::Value::Interval(interval);
      }
      break;
    case googlesql::TYPE_JSON:
      if (id == DUCKDB_TYPE_VARCHAR) {
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
  chunks->Runs(row, 1, [&](duckdb_data_chunk chunk, size_t index, size_t, size_t) {
    value = ToGoogleSqlValue(duckdb_data_chunk_get_vector(chunk, column), chunks->type(column),
                             index, type, rows.null_arrays());
  });
  return value;
}

}  // namespace bigquery_emulator_duckdb
