#include "src/googlesql_functions_duckdb/cast.h"

#include "duckdb.h"
#include "src/duckdb_functions/apply.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_functions/cast.h"

namespace bigquery_emulator_duckdb::functions_duckdb {
using namespace duckdb_functions;
using namespace googlesql_functions;

void RegisterCastFunctions(duckdb_connection connection) {
  constexpr Errors kSafe = Errors::kNullUnderSafeCast;
  LogicalType const numeric(duckdb_create_decimal_type(38, 9));
  LogicalType const varchar(duckdb_create_logical_type(kVarchar));
  LogicalType const boolean(duckdb_create_logical_type(kBoolean));
  Register(connection, "bq_bytes_to_string", {kBlob, kBoolean}, kVarchar,
           Apply<CastBytesToUtf8, kSafe>);
  Register(connection, "bq_float64_to_int64", {kDouble, kBoolean}, kBigint,
           Apply<CastFloat64ToInt64, kSafe>);
  Register(connection, "bq_string_to_bool", {kVarchar, kBoolean}, kBoolean,
           Apply<CastStringToBool, kSafe>);
  // Each cast with FORMAT takes the value, the format, the time zone for a cast from or to
  // TIMESTAMP, the current timestamp for a cast to DATE, DATETIME or TIMESTAMP and whether it is
  // SAFE_CAST.
  Register(connection, "bq_cast_bytes_to_string", {kBlob, kVarchar, kBoolean}, kVarchar,
           Apply<CastBytesToString, kSafe>);
  Register(connection, "bq_cast_date_to_string", {kDate, kVarchar, kBoolean}, kVarchar,
           Apply<CastDateToString, kSafe>);
  Register(connection, "bq_cast_datetime_to_string", {kDatetime, kVarchar, kBoolean}, kVarchar,
           Apply<CastDatetimeToString, kSafe>);
  Register(connection, "bq_cast_time_to_string", {kTime, kVarchar, kBoolean}, kVarchar,
           Apply<CastTimeToString, kSafe>);
  Register(connection, "bq_cast_timestamp_to_string", {kTimestamp, kVarchar, kVarchar, kBoolean},
           kVarchar, Apply<CastTimestampToString, kSafe>);
  Register(connection, "bq_cast_int64_to_string", {kBigint, kVarchar, kBoolean}, kVarchar,
           Apply<CastInt64ToString, kSafe>);
  Register(connection, "bq_cast_float64_to_string", {kDouble, kVarchar, kBoolean}, kVarchar,
           Apply<CastFloat64ToString, kSafe>);
  Register(connection, "bq_cast_numeric_to_string", {numeric.get(), varchar.get(), boolean.get()},
           varchar.get(), Apply<CastNumericToString, kSafe>);
  // A BIGNUMERIC is the VARCHAR of its units; see src/bignumeric.h.
  Register(connection, "bq_cast_bignumeric_to_string", {kVarchar, kVarchar, kBoolean}, kVarchar,
           Apply<CastBigNumericToString, kSafe>);
  Register(connection, "bq_cast_string_to_bytes", {kVarchar, kVarchar, kBoolean}, kBlob,
           Apply<CastStringToBytes, kSafe>);
  Register(connection, "bq_cast_string_to_date", {kVarchar, kVarchar, kTimestamp, kBoolean}, kDate,
           Apply<CastStringToDate, kSafe>);
  Register(connection, "bq_cast_string_to_datetime", {kVarchar, kVarchar, kTimestamp, kBoolean},
           kDatetime, Apply<CastStringToDatetime, kSafe>);
  Register(connection, "bq_cast_string_to_time", {kVarchar, kVarchar, kBoolean}, kTime,
           Apply<CastStringToTime, kSafe>);
  Register(connection, "bq_cast_string_to_timestamp",
           {kVarchar, kVarchar, kVarchar, kTimestamp, kBoolean}, kTimestamp,
           Apply<CastStringToTimestamp, kSafe>);
}

}  // namespace bigquery_emulator_duckdb::functions_duckdb
