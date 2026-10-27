#include "src/googlesql_functions_duckdb/datetime.h"

#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "src/bignumeric.h"
#include "src/duckdb_functions/apply.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_functions/datetime.h"
#include "src/googlesql_functions/format.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/interval.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::functions_duckdb {
using namespace duckdb_functions;
using namespace googlesql_functions;

namespace {

// The GoogleSQL type of a FORMAT value, which the handler passes as the DuckDB type that stands
// for it: JSON as a VARCHAR by its alias, a RANGE or a STRUCT<> as the STRUCT IsRange or
// IsEmptyStruct tells from others, and an ARRAY as a LIST. Arrays and structs are made in
// `factory`; other types are unsupported.
absl::StatusOr<const googlesql::Type*> FormatType(duckdb_logical_type type,
                                                  googlesql::TypeFactory& factory) {
  namespace types = googlesql::types;
  const DuckString alias(duckdb_logical_type_get_alias(type));
  if (alias != nullptr && std::string_view(alias.get()) == "JSON") {
    return types::JsonType();
  }
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_VARCHAR:
      return types::StringType();
    case DUCKDB_TYPE_BLOB:
      return types::BytesType();
    case DUCKDB_TYPE_BIGINT:
      return types::Int64Type();
    case DUCKDB_TYPE_DOUBLE:
      return types::DoubleType();
    case DUCKDB_TYPE_BOOLEAN:
      return types::BoolType();
    case DUCKDB_TYPE_DATE:
      return types::DateType();
    case DUCKDB_TYPE_TIME:
      return types::TimeType();
    case DUCKDB_TYPE_TIMESTAMP:
      return types::DatetimeType();
    case DUCKDB_TYPE_TIMESTAMP_TZ:
      return types::TimestampType();
    case DUCKDB_TYPE_BIGNUM:
      return types::BigNumericType();
    case DUCKDB_TYPE_DECIMAL:
      return types::NumericType();
    case DUCKDB_TYPE_INTERVAL:
      return types::IntervalType();
    case DUCKDB_TYPE_STRUCT: {
      if (IsRange(type)) {
        LogicalType const element(duckdb_struct_type_child_type(type, 0));
        const duckdb_type element_type = duckdb_get_type_id(element.get());
        return element_type == kDate       ? types::DateRangeType()
               : element_type == kDatetime ? types::DatetimeRangeType()
                                           : types::TimestampRangeType();
      }
      // A STRUCT<> keeps a placeholder field, which FORMAT does not write.
      if (IsEmptyStruct(type)) {
        return types::EmptyStructType();
      }
      // FORMAT writes no field names.
      std::vector<googlesql::StructField> fields;
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        LogicalType const field(duckdb_struct_type_child_type(type, i));
        GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::Type* field_type,
                                   FormatType(field.get(), factory));
        fields.emplace_back("", field_type);
      }
      const googlesql::StructType* struct_type = nullptr;
      GOOGLESQL_RETURN_IF_ERROR(factory.MakeStructType(fields, &struct_type));
      return struct_type;
    }
    case DUCKDB_TYPE_LIST: {
      LogicalType const element(duckdb_list_type_child_type(type));
      GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::Type* element_type,
                                 FormatType(element.get(), factory));
      return factory.MakeArrayType(element_type);
    }
    default:
      return absl::UnimplementedError("Unsupported argument type for FORMAT");
  }
}

// The FORMAT value of `type` in row `row` of `vector`, whose DuckDB type FormatType maps to it.
absl::StatusOr<googlesql::Value> FormatValue(duckdb_vector vector, idx_t row,
                                             const googlesql::Type* type) {
  uint64_t* validity = duckdb_vector_get_validity(vector);
  if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
    return googlesql::Value::Null(type);
  }
  switch (type->kind()) {
    case googlesql::TYPE_JSON: {
      // Parsed as the JSON functions parse it.
      GOOGLESQL_ASSIGN_OR_RETURN(auto json,
                                 googlesql::JSONValue::ParseJSONString(VectorString(vector, row)));
      return googlesql::Value::Json(std::move(json));
    }
    case googlesql::TYPE_STRING:
      return googlesql::Value::String(VectorString(vector, row));
    case googlesql::TYPE_BYTES:
      return googlesql::Value::Bytes(VectorString(vector, row));
    case googlesql::TYPE_INT64:
      return googlesql::Value::Int64(VectorElement<int64_t>(vector, row));
    case googlesql::TYPE_DOUBLE:
      return googlesql::Value::Double(VectorElement<double>(vector, row));
    case googlesql::TYPE_BOOL:
      return googlesql::Value::Bool(VectorElement<bool>(vector, row));
    case googlesql::TYPE_DATE:
      return googlesql::Value::Date(VectorElement<int32_t>(vector, row));
    case googlesql::TYPE_TIME:
      return googlesql::Value::Time(TimeFromMicros(VectorElement<int64_t>(vector, row)));
    case googlesql::TYPE_DATETIME: {
      GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime,
                                 DatetimeFromMicros(VectorElement<int64_t>(vector, row)));
      return googlesql::Value::Datetime(datetime);
    }
    case googlesql::TYPE_TIMESTAMP:
      return googlesql::Value::Timestamp(absl::FromUnixMicros(VectorElement<int64_t>(vector, row)));
    case googlesql::TYPE_BIGNUMERIC: {
      GOOGLESQL_ASSIGN_OR_RETURN(const auto bignumeric,
                                 BigNumericFromBignum(VectorString(vector, row)));
      return googlesql::Value::BigNumeric(bignumeric);
    }
    case googlesql::TYPE_NUMERIC: {
      const auto value = VectorElement<duckdb_hugeint>(vector, row);
      GOOGLESQL_ASSIGN_OR_RETURN(
          const auto numeric,
          googlesql::NumericValue::FromPackedInt(std::bit_cast<__int128>(
              (static_cast<unsigned __int128>(value.upper) << 64U) | value.lower)));
      return googlesql::Value::Numeric(numeric);
    }
    case googlesql::TYPE_INTERVAL: {
      GOOGLESQL_ASSIGN_OR_RETURN(const auto interval,
                                 IntervalFromDuckDb(VectorElement<duckdb_interval>(vector, row)));
      return googlesql::Value::Interval(interval);
    }
    case googlesql::TYPE_RANGE: {
      // The unbounded ends are infinite in DuckDB and NULL in GoogleSQL.
      const googlesql::Type* element = type->AsRange()->element_type();
      std::vector<googlesql::Value> bounds;
      for (idx_t i = 0; i < 2; ++i) {
        duckdb_vector child = duckdb_struct_vector_get_child(vector, i);
        const bool finite = element->IsDate()
                                ? duckdb_is_finite_date({VectorElement<int32_t>(child, row)})
                                : duckdb_is_finite_timestamp({VectorElement<int64_t>(child, row)});
        if (finite) {
          GOOGLESQL_ASSIGN_OR_RETURN(auto bound, FormatValue(child, row, element));
          bounds.push_back(std::move(bound));
        } else {
          bounds.push_back(googlesql::Value::Null(element));
        }
      }
      return googlesql::Value::MakeRange(bounds.at(0), bounds.at(1));
    }
    case googlesql::TYPE_ARRAY: {
      const auto entry = VectorElement<duckdb_list_entry>(vector, row);
      duckdb_vector child = duckdb_list_vector_get_child(vector);
      std::vector<googlesql::Value> elements;
      elements.reserve(entry.length);
      for (idx_t i = 0; i < entry.length; ++i) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            auto element, FormatValue(child, entry.offset + i, type->AsArray()->element_type()));
        elements.push_back(std::move(element));
      }
      return googlesql::Value::MakeArray(type->AsArray(), std::move(elements));
    }
    case googlesql::TYPE_STRUCT: {
      std::vector<googlesql::Value> fields;
      for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
        GOOGLESQL_ASSIGN_OR_RETURN(auto field,
                                   FormatValue(duckdb_struct_vector_get_child(vector, i), row,
                                               type->AsStruct()->field(i).type));
        fields.push_back(std::move(field));
      }
      return googlesql::Value::MakeStruct(type->AsStruct(), std::move(fields));
    }
    default:
      return absl::UnimplementedError("Unsupported argument type for FORMAT");
  }
}

// FORMAT(format, values...), which is NULL for a NULL format and where GoogleSQL says so for
// a NULL value.
void Format(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  googlesql::TypeFactory factory;
  std::vector<const googlesql::Type*> types;
  for (idx_t column = 1; column < duckdb_data_chunk_get_column_count(input); ++column) {
    LogicalType const type(
        duckdb_vector_get_column_type(duckdb_data_chunk_get_vector(input, column)));
    const auto format_type = FormatType(type.get(), factory);
    if (!format_type.ok()) {
      duckdb_scalar_function_set_error(info, std::string(format_type.status().message()).c_str());
      return;
    }
    types.push_back(*format_type);
  }
  const auto& options = FunctionContext<googlesql_functions::FormatOptions>(info);
  EachRow(
      info, input, output,
      [&types, &options](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        std::vector<googlesql::Value> values;
        for (idx_t column = 1; column <= types.size(); ++column) {
          auto value = FormatValue(arguments.Vector(column), arguments.Row(), types.at(column - 1));
          if (!value.ok()) {
            return value.status();
          }
          values.push_back(*std::move(value));
        }
        return googlesql_functions::Format(arguments.String(0), values, options);
      },
      /*nulls=*/false);
}

}  // namespace

void RegisterDatetimeFunctions(duckdb_connection connection, const Options& options) {
  Register(connection, "bq_months_between_date", {kDate, kDate}, kDouble, Apply<MonthsBetweenDate>);
  Register(connection, "bq_months_between_datetime", {kDatetime, kDatetime}, kDouble,
           Apply<MonthsBetweenDatetime>);
  RegisterWithContext(connection, "bq_format_date", {kVarchar, kDate}, kVarchar,
                      ApplyWithContext<FormatDate>, options.unpadded_iso_year);
  RegisterWithContext(connection, "bq_format_datetime", {kVarchar, kDatetime}, kVarchar,
                      ApplyWithContext<FormatDatetime>, options.unpadded_iso_year);
  Register(connection, "bq_format_time", {kVarchar, kTime}, kVarchar, Apply<FormatTime>);
  RegisterWithContext(connection, "bq_format_timestamp", {kVarchar, kTimestamp, kVarchar}, kVarchar,
                      ApplyWithContext<FormatTimestamp>, options.unpadded_iso_year);
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
  Register(connection, "bq_timestamp_to_datetime_in", {kTimestamp, kVarchar}, kDatetime,
           Apply<TimestampToDatetimeIn>);
  Register(connection, "bq_timestamp_to_time_in", {kTimestamp, kVarchar}, kTime,
           Apply<TimestampToTimeIn>);
  Register(connection, "bq_timestamp_civil_in", {kTimestamp, kVarchar}, kDatetime,
           Apply<TimestampCivilIn>);
  Register(connection, "bq_datetime_to_timestamp_in", {kDatetime, kVarchar}, kTimestamp,
           Apply<DatetimeToTimestampIn>);
  Register(connection, "bq_timestamp_trunc", {kTimestamp, kVarchar, kVarchar}, kTimestamp,
           Apply<TimestampTrunc>);
  LogicalType const date(duckdb_create_logical_type(kDate));
  LogicalType const dates(duckdb_create_list_type(date.get()));
  RegisterWithContext(connection, "bq_generate_date_array", {kDate, kDate, kBigint, kVarchar},
                      dates.get(), ApplyWithContext<GenerateDateArray>,
                      options.extended_array_limits);
  Register(connection, "bq_datetime_string", {kDatetime}, kVarchar, Apply<DatetimeToString>);
  Register(connection, "bq_time_string", {kTime}, kVarchar, Apply<TimeToString>);
  Register(connection, "bq_timestamp_string", {kTimestamp, kVarchar}, kVarchar,
           Apply<TimestampToString>);
  RegisterWithContext(connection, "bq_format", {kVarchar}, kVarchar, Format,
                      googlesql_functions::FormatOptions{
                          .adapt_specifiers = options.format_specifiers,
                          .unsigned_nan = options.unsigned_nan,
                          .signed_zero = options.signed_zero_strings,
                      },
                      /*nulls=*/false, DUCKDB_TYPE_ANY);
}

}  // namespace bigquery_emulator_duckdb::functions_duckdb
