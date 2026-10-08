#include "src/backend_functions/interval.h"

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/functions/datetime.pb.h"
#include "googlesql/public/interval_value.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/interval.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

constexpr duckdb_type kInterval = DUCKDB_TYPE_INTERVAL;

absl::StatusOr<googlesql::IntervalValue> ReadInterval(const Arguments& args) {
  return IntervalFromDuckDb(VectorElement<duckdb_interval>(args.Vector(0), args.Row()));
}

void MakeInterval(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<duckdb_interval> {
    auto value = googlesql::IntervalValue::FromYMDHMS(args.Int(0), args.Int(1), args.Int(2),
                                                      args.Int(3), args.Int(4), args.Int(5));
    if (!value.ok()) {
      // BigQuery names the function after GoogleSQL's message.
      return absl::Status(
          value.status().code(),
          absl::StrCat(value.status().message(), "; error in MAKE_INTERVAL expression"));
    }
    return IntervalToDuckDb(*value);
  });
}

// Resolved literals already carry validated components; this also validates the physical value.
void IntervalParts(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<duckdb_interval> {
    GOOGLESQL_ASSIGN_OR_RETURN(auto value, googlesql::IntervalValue::FromMonthsDaysMicros(
                                               args.Int(0), args.Int(1), args.Int(2)));
    return IntervalToDuckDb(value);
  });
}

void IntervalConstructor(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<duckdb_interval> {
    googlesql::functions::DateTimestampPart part = googlesql::functions::DATE;
    if (!googlesql::functions::DateTimestampPart_Parse(args.String(1), &part)) {
      return absl::InvalidArgumentError("Unsupported INTERVAL date part");
    }
    GOOGLESQL_ASSIGN_OR_RETURN(auto value,
                               googlesql::IntervalValue::FromInteger(args.Int(0), part, false));
    return IntervalToDuckDb(value);
  });
}

template <auto Justify>
void JustifyInterval(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<duckdb_interval> {
    GOOGLESQL_ASSIGN_OR_RETURN(auto value, ReadInterval(args));
    GOOGLESQL_ASSIGN_OR_RETURN(auto result, Justify(value));
    return IntervalToDuckDb(result);
  });
}

void IntervalString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<std::string> {
    GOOGLESQL_ASSIGN_OR_RETURN(auto value, ReadInterval(args));
    return value.ToString();
  });
}

}  // namespace

void RegisterIntervalFunctions(duckdb_connection connection) {
  Register(connection, "bq_make_interval", {kBigint, kBigint, kBigint, kBigint, kBigint, kBigint},
           kInterval, MakeInterval);
  Register(connection, "bq_interval_parts", {kBigint, kBigint, kBigint}, kInterval, IntervalParts);
  Register(connection, "bq_interval", {kBigint, kVarchar}, kInterval, IntervalConstructor);
  Register(connection, "bq_justify_hours", {kInterval}, kInterval,
           JustifyInterval<googlesql::JustifyHours>);
  Register(connection, "bq_justify_days", {kInterval}, kInterval,
           JustifyInterval<googlesql::JustifyDays>);
  Register(connection, "bq_justify_interval", {kInterval}, kInterval,
           JustifyInterval<googlesql::JustifyInterval>);
  Register(connection, "bq_interval_string", {kInterval}, kVarchar, IntervalString);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
