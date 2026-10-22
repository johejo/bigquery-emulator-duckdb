#include "src/backend_functions/range.h"

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

// GoogleSQL's error, which BigQuery names the function after.
absl::Status InExpression(const absl::Status& status) {
  if (status.ok()) {
    return status;
  }
  return {status.code(),
          absl::StrCat(status.message(), "; error in GENERATE_RANGE_ARRAY expression")};
}

// The bounds of a RANGE<DATE> as days since the epoch.
struct DateBounds {
  using Bound = int32_t;
  using Result = int32_t;

  static absl::StatusOr<std::optional<Bound>> Read(const Arguments& args, idx_t column) {
    const int32_t days = args.Date(column);
    return duckdb_is_finite_date(duckdb_date{.days = days}) ? std::optional(days) : std::nullopt;
  }
  static auto Generator(const googlesql::IntervalValue& step, bool last_partial_range) {
    return fn::DateRangeArrayGenerator::Create(step, last_partial_range);
  }
  static absl::StatusOr<Result> Write(Bound days) { return days; }
};

// The bounds of a RANGE<DATETIME> as microseconds of civil time.
struct DatetimeBounds {
  using Bound = googlesql::DatetimeValue;
  using Result = int64_t;

  static absl::StatusOr<std::optional<Bound>> Read(const Arguments& args, idx_t column) {
    const int64_t micros = args.Int(column);
    if (!duckdb_is_finite_timestamp(duckdb_timestamp{.micros = micros})) {
      return std::nullopt;
    }
    return DatetimeFromMicros(micros);
  }
  static auto Generator(const googlesql::IntervalValue& step, bool last_partial_range) {
    return fn::DatetimeRangeArrayGenerator::Create(step, last_partial_range, fn::kMicroseconds);
  }
  static absl::StatusOr<Result> Write(const Bound& datetime) {
    return MicrosFromDatetime(datetime);
  }
};

// The bounds of a RANGE<TIMESTAMP> as microseconds since the epoch.
struct TimestampBounds {
  using Bound = absl::Time;
  using Result = int64_t;

  static absl::StatusOr<std::optional<Bound>> Read(const Arguments& args, idx_t column) {
    const int64_t micros = args.Int(column);
    return duckdb_is_finite_timestamp(duckdb_timestamp{.micros = micros})
               ? std::optional(absl::FromUnixMicros(micros))
               : std::nullopt;
  }
  static auto Generator(const googlesql::IntervalValue& step, bool last_partial_range) {
    return fn::TimestampRangeArrayGenerator::Create(step, last_partial_range, fn::kMicroseconds);
  }
  static absl::StatusOr<Result> Write(Bound time) { return absl::ToUnixMicros(time); }
};

// GENERATE_RANGE_ARRAY over the bounds of a RANGE, an infinite one being UNBOUNDED, as the bounds
// of the subranges, which the rule pairs into RANGEs.
template <typename Bounds>
void GenerateRanges(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& args) -> absl::StatusOr<std::vector<typename Bounds::Result>> {
        GOOGLESQL_ASSIGN_OR_RETURN(const auto start, Bounds::Read(args, 0));
        GOOGLESQL_ASSIGN_OR_RETURN(const auto end, Bounds::Read(args, 1));
        GOOGLESQL_ASSIGN_OR_RETURN(const auto step, Codec<googlesql::IntervalValue>::Read(args, 2));
        const auto generator = Bounds::Generator(step, args.Bool(3));
        GOOGLESQL_RETURN_IF_ERROR(InExpression(generator.status()));
        std::vector<typename Bounds::Bound> bounds;
        GOOGLESQL_RETURN_IF_ERROR(InExpression(generator->Generate(start, end, Append(bounds))));
        std::vector<typename Bounds::Result> results;
        results.reserve(bounds.size());
        for (const auto& bound : bounds) {
          GOOGLESQL_ASSIGN_OR_RETURN(results.emplace_back(), Bounds::Write(bound));
        }
        return results;
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
           std::pair{kDate, &GenerateRanges<DateBounds>},
           std::pair{kDatetime, &GenerateRanges<DatetimeBounds>},
           std::pair{kTimestamp, &GenerateRanges<TimestampBounds>},
       }) {
    LogicalType const type(duckdb_create_logical_type(element));
    LogicalType const list(duckdb_create_list_type(type.get()));
    Register(connection, "bq_generate_range_array", {element, element, kInterval, kBoolean},
             list.get(), function);
  }
}

}  // namespace bigquery_emulator_duckdb::backend_functions
