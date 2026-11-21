#include "src/duckdb_functions/float_range.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::duckdb_functions {
namespace {

constexpr __int128 kNan = -static_cast<__int128>(static_cast<unsigned __int128>(1) << 65U);
constexpr __int128 kInfinity = static_cast<__int128>(0x7ff0000000000000ULL) * 2;

// IEEE magnitudes are ordered for nonnegative doubles. Negating the magnitude orders negative
// doubles too, and folds -0 into +0. Leave a gap between adjacent keys: a finite boundary past
// DOUBLE's limits must lie strictly between the finite values and infinity, not include infinity.
__int128 Key(double value) {
  if (std::isnan(value)) {
    return kNan;
  }
  const auto magnitude = std::bit_cast<uint64_t>(value) & 0x7fffffffffffffffULL;
  const __int128 key = static_cast<__int128>(magnitude) * 2;
  return std::signbit(value) ? -key : key;
}

double Value(__int128 key) {
  if (key == kNan) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const auto magnitude = static_cast<uint64_t>((key < 0 ? -key : key) / 2);
  return std::bit_cast<double>(magnitude | (key < 0 ? 0x8000000000000000ULL : 0));
}

void RangeKey(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& args) -> absl::StatusOr<__int128> { return Key(args.Double(0)); });
}

void RangeOffset(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<double> {
    const double offset = args.Double(0);
    if (!(offset >= 0)) {
      return absl::OutOfRangeError(
          "Window frame offset for PRECEDING or FOLLOWING must be non-negative");
    }
    return offset;
  });
}

template <bool kSubtract>
void RangeAdd(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<__int128> {
    const double value = Value(args.Int128(0));
    auto* const child = duckdb_struct_vector_get_child(args.Vector(1), 0);
    const double offset = static_cast<const double*>(duckdb_vector_get_data(child))[args.Row()];
    if (std::isnan(value)) {
      return kNan;
    }
    // An infinite offset cannot move an infinity of the opposite sign in a single direction.
    if (std::isinf(offset) && (kSubtract ? value > 0 : value < 0) && std::isinf(value)) {
      return absl::OutOfRangeError(
          "Offset value in OVER(RANGE ... <offset> PRECEDING|FOLLOWING) cannot be infinity "
          "when there exists an infinity order key making the window boundary value NaN.");
    }
    if (std::isfinite(value) && std::isfinite(offset)) {
      constexpr double max = std::numeric_limits<double>::max();
      if (kSubtract ? value < -max + offset : value > max - offset) {
        return kSubtract ? -kInfinity + 1 : kInfinity - 1;
      }
    }
    return Key(kSubtract ? value - offset : value + offset);
  });
}

}  // namespace

void RegisterFloatRangeFunctions(duckdb_connection connection) {
  Register(connection, "bq_float_range_key", {kDouble}, DUCKDB_TYPE_HUGEINT, RangeKey);
  Register(connection, "bq_float_range_offset", {kDouble}, kDouble, RangeOffset);
  LogicalType const key(duckdb_create_logical_type(DUCKDB_TYPE_HUGEINT));
  LogicalType const value(duckdb_create_logical_type(kDouble));
  duckdb_logical_type fields[] = {value.get()};
  const char* names[] = {"bq_float_range_offset"};
  LogicalType const offset(duckdb_create_struct_type(fields, names, 1));
  const std::vector<duckdb_logical_type> parameters = {key.get(), offset.get()};
  // The tagged offset prevents these overloads from changing ordinary numeric arithmetic.
  Register(connection, "+", parameters, key.get(), RangeAdd<false>);
  Register(connection, "-", parameters, key.get(), RangeAdd<true>);
}

}  // namespace bigquery_emulator_duckdb::duckdb_functions
