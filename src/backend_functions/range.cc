#include "src/backend_functions/range.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/functions/range.h"
#include "googlesql/public/interval_value.h"
#include "src/backend_functions/apply.h"
#include "src/backend_functions/datetime.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/interval.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

namespace fn = googlesql::functions;

// The bounds of the text of a RANGE, which CAST to RANGE then casts to the element type, each
// NULL where it is unbounded.
absl::StatusOr<std::vector<std::optional<std::string>>> RangeBounds(std::string_view text) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto bounds, googlesql::ParseRangeBoundaries(text));
  return std::vector<std::optional<std::string>>{
      bounds.start ? std::optional<std::string>(*bounds.start) : std::nullopt,
      bounds.end ? std::optional<std::string>(*bounds.end) : std::nullopt,
  };
}

// An emitter that collects the bounds of the subranges a generator emits, in order: the first
// one's start, then each one's end, which is the next one's start.
template <typename Bound>
auto Append(std::vector<Bound>& bounds) {
  return [&bounds](const Bound& start, const Bound& end) {
    if (bounds.empty()) {
      bounds.push_back(start);
    }
    bounds.push_back(end);
    return absl::OkStatus();
  };
}

absl::StatusOr<googlesql::IntervalValue> Step(const Arguments& args) {
  return IntervalFromDuckDb(VectorElement<duckdb_interval>(args.Vector(2), args.Row()));
}

// GoogleSQL's error, which BigQuery names the function after.
absl::Status InExpression(const absl::Status& status) {
  if (status.ok()) {
    return status;
  }
  return {status.code(),
          absl::StrCat(status.message(), "; error in GENERATE_RANGE_ARRAY expression")};
}

// GENERATE_RANGE_ARRAY over the bounds of a RANGE<DATE>, an infinite one being UNBOUNDED, as
// the bounds of the subranges, which the rule pairs into RANGEs.
void GenerateDateRanges(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<std::vector<int32_t>> {
    const auto bound = [&](idx_t column) -> std::optional<int32_t> {
      const int32_t days = args.Date(column);
      return duckdb_is_finite_date(duckdb_date{.days = days}) ? std::optional(days) : std::nullopt;
    };
    GOOGLESQL_ASSIGN_OR_RETURN(const auto step, Step(args));
    const auto generator = fn::DateRangeArrayGenerator::Create(step, args.Bool(3));
    GOOGLESQL_RETURN_IF_ERROR(InExpression(generator.status()));
    std::vector<int32_t> bounds;
    GOOGLESQL_RETURN_IF_ERROR(
        InExpression(generator->Generate(bound(0), bound(1), Append(bounds))));
    return bounds;
  });
}

// As GenerateDateRanges for a RANGE<DATETIME>, whose bounds are microseconds of civil time.
void GenerateDatetimeRanges(duckdb_function_info info, duckdb_data_chunk input,
                            duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<std::vector<int64_t>> {
    const auto bound =
        [&](idx_t column) -> absl::StatusOr<std::optional<googlesql::DatetimeValue>> {
      const int64_t micros = args.Int(column);
      if (!duckdb_is_finite_timestamp(duckdb_timestamp{.micros = micros})) {
        return std::nullopt;
      }
      return DatetimeFromMicros(micros);
    };
    GOOGLESQL_ASSIGN_OR_RETURN(const auto start, bound(0));
    GOOGLESQL_ASSIGN_OR_RETURN(const auto end, bound(1));
    GOOGLESQL_ASSIGN_OR_RETURN(const auto step, Step(args));
    const auto generator =
        fn::DatetimeRangeArrayGenerator::Create(step, args.Bool(3), fn::kMicroseconds);
    GOOGLESQL_RETURN_IF_ERROR(InExpression(generator.status()));
    std::vector<googlesql::DatetimeValue> bounds;
    GOOGLESQL_RETURN_IF_ERROR(InExpression(generator->Generate(start, end, Append(bounds))));
    std::vector<int64_t> micros;
    micros.reserve(bounds.size());
    for (const auto& datetime : bounds) {
      GOOGLESQL_ASSIGN_OR_RETURN(micros.emplace_back(), MicrosFromDatetime(datetime));
    }
    return micros;
  });
}

// As GenerateDateRanges for a RANGE<TIMESTAMP>.
void GenerateTimestampRanges(duckdb_function_info info, duckdb_data_chunk input,
                             duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<std::vector<int64_t>> {
    const auto bound = [&](idx_t column) -> std::optional<absl::Time> {
      const int64_t micros = args.Int(column);
      return duckdb_is_finite_timestamp(duckdb_timestamp{.micros = micros})
                 ? std::optional(absl::FromUnixMicros(micros))
                 : std::nullopt;
    };
    GOOGLESQL_ASSIGN_OR_RETURN(const auto step, Step(args));
    const auto generator =
        fn::TimestampRangeArrayGenerator::Create(step, args.Bool(3), fn::kMicroseconds);
    GOOGLESQL_RETURN_IF_ERROR(InExpression(generator.status()));
    std::vector<absl::Time> bounds;
    GOOGLESQL_RETURN_IF_ERROR(
        InExpression(generator->Generate(bound(0), bound(1), Append(bounds))));
    std::vector<int64_t> micros(bounds.size());
    std::ranges::transform(bounds, micros.begin(), absl::ToUnixMicros);
    return micros;
  });
}

}  // namespace

void RegisterRangeFunctions(duckdb_connection connection) {
  LogicalType const text(duckdb_create_logical_type(kVarchar));
  LogicalType const texts(duckdb_create_list_type(text.get()));
  Register(connection, "bq_range_bounds", {kVarchar, kBoolean}, texts.get(),
           Apply<RangeBounds, Errors::kNullUnderSafeCast>);
  // One name for each element type, so that DuckDB picks the overload.
  constexpr duckdb_type kInterval = DUCKDB_TYPE_INTERVAL;
  for (const auto& [element, function] : {
           std::pair{kDate, GenerateDateRanges},
           std::pair{kDatetime, GenerateDatetimeRanges},
           std::pair{kTimestamp, GenerateTimestampRanges},
       }) {
    LogicalType const type(duckdb_create_logical_type(element));
    LogicalType const list(duckdb_create_list_type(type.get()));
    Register(connection, "bq_generate_range_array", {element, element, kInterval, kBoolean},
             list.get(), function);
  }
}

}  // namespace bigquery_emulator_duckdb::backend_functions
