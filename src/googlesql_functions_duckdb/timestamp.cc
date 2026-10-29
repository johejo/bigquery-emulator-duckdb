#include "src/googlesql_functions_duckdb/timestamp.h"

#include <cstdint>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "src/backend_error.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/timestamp_nanos.h"
#include "src/timestamp_precision.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

void RegisterTimestampNanosFunctions(duckdb_connection connection) {
  using namespace duckdb_functions;
  LogicalType const type(duckdb_create_logical_type(DUCKDB_TYPE_HUGEINT));
  duckdb_logical_type_set_alias(type.get(), kTimestampNanosType);
  if (duckdb_register_logical_type(connection, type.get(), nullptr) == DuckDBError) {
    throw BackendError("Cannot register the nanosecond TIMESTAMP type");
  }
  LogicalType const text_type(duckdb_create_logical_type(kVarchar));
  LogicalType const micros_type(duckdb_create_logical_type(kTimestamp));
  Register(
      connection, "bq_timestamp_nanos_text", {type.get()}, text_type.get(),
      +[](duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
        EachRow(info, input, output, [](const Arguments& arguments) {
          return TimestampNanosText(arguments.Decimal(0));
        });
      });
  Register(
      connection, "bq_timestamp_nanos_from_string", {kVarchar}, type.get(),
      +[](duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
        EachRow(info, input, output, [](const Arguments& arguments) {
          return ParseTimestampNanos(arguments.StringView(0));
        });
      });
  Register(
      connection, "bq_timestamp_nanos_from_micros", {kTimestamp}, type.get(),
      +[](duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
        EachRow(info, input, output, [](const Arguments& arguments) {
          return TimestampNanos(absl::FromUnixMicros(arguments.Int(0)));
        });
      });
  Register(
      connection, "bq_timestamp_nanos_to_micros", {type.get()}, micros_type.get(),
      +[](duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
        EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<int64_t> {
          const __int128 nanos = arguments.Decimal(0);
          GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time time, TimestampFromNanos(nanos));
          if (nanos % 1000 != 0) {
            return absl::UnimplementedError(
                "This operation does not support nanosecond TIMESTAMP precision");
          }
          return absl::ToUnixMicros(time);
        });
      });
}

}  // namespace bigquery_emulator_duckdb::functions_duckdb
