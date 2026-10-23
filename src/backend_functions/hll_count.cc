#include "src/backend_functions/hll_count.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "src/backend_error.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/bignumeric.h"
#include "src/duckdb_handle.h"
#include "src/zetasketch/fingerprint2011.h"
#include "src/zetasketch/hyperloglog_plus_plus.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

using zetasketch::HyperLogLogPlusPlus;

// The value types BigQuery's sketches carry: DefaultOpsType UINT64 for INT64,
// BYTES_OR_UTF8_STRING for STRING and BYTES, and custom types for NUMERIC and BIGNUMERIC.
enum class ValueType : int32_t {
  kInt64 = 8,
  kBytes = 11,
  kNumeric = 188553293,
  kBigNumeric = 188553294,
};

constexpr int kDefaultSparsePrecisionDelta = 5;

constexpr char kInvalidSketch[] = "Invalid or incompatible sketch in HLL_COUNT.MERGE_PARTIAL";

// The little-endian two's complement bytes of `value`, in `size` bytes.
template <typename Integer>
std::string LittleEndian(Integer value, std::size_t size) {
  auto bits = static_cast<std::make_unsigned_t<Integer>>(value);
  std::string bytes(size, '\0');
  for (char& byte : bytes) {
    byte = static_cast<char>(bits & 0xFFU);
    bits >>= 8U;
  }
  return bytes;
}

// The hash of row `row` of `vector`, a value of `type`: Fingerprint2011 of its bytes, of an INT64's
// 8 little-endian bytes, and of the little-endian two's complement of a NUMERIC's or BIGNUMERIC's
// units, in 16 or 32 bytes.
absl::StatusOr<uint64_t> Hash(ValueType type, duckdb_vector vector, idx_t row) {
  switch (type) {
    case ValueType::kInt64:
      return zetasketch::Fingerprint2011(LittleEndian(VectorElement<int64_t>(vector, row), 8));
    case ValueType::kBytes:
      return zetasketch::Fingerprint2011(VectorString(vector, row));
    case ValueType::kNumeric: {
      const auto value = VectorElement<duckdb_hugeint>(vector, row);
      std::string bytes = LittleEndian(value.lower, 8);
      bytes += LittleEndian(value.upper, 8);
      return zetasketch::Fingerprint2011(bytes);
    }
    case ValueType::kBigNumeric: {
      const auto value = BigNumericFromUnits(VectorString(vector, row));
      if (!value.ok()) {
        return value.status();
      }
      std::string bytes = BigNumericDecimalBytes(*value);
      std::ranges::reverse(bytes);
      return zetasketch::Fingerprint2011(bytes);
    }
  }
  return 0;
}

using Sketch = std::optional<HyperLogLogPlusPlus>;

Sketch*& State(duckdb_aggregate_state state) { return *reinterpret_cast<Sketch**>(state); }

idx_t StateSize(duckdb_function_info /*info*/) { return sizeof(Sketch*); }

void InitState(duckdb_function_info /*info*/, duckdb_aggregate_state state) {
  State(state) = new Sketch();
}

void DestroyStates(duckdb_aggregate_state* states, idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    delete State(states[i]);
  }
}

// Merges `source` into `target`, or returns the error MERGE_PARTIAL fails with.
const char* MergeInto(Sketch& target, const HyperLogLogPlusPlus& source) {
  if (!target) {
    target = source;
    return nullptr;
  }
  switch (target->Merge(source)) {
    case HyperLogLogPlusPlus::MergeResult::kMerged:
      return nullptr;
    case HyperLogLogPlusPlus::MergeResult::kIncompatibleTypes:
      return "HLL_COUNT.MERGE_PARTIAL failed to compute.";
    case HyperLogLogPlusPlus::MergeResult::kIncompatiblePrecisions:
      return kInvalidSketch;
  }
  return nullptr;
}

// HLL_COUNT.INIT adds each non-NULL value, argument 0, to a sketch of the precision in argument
// 1, which the first value sets.
template <ValueType kType>
void AddValues(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state* states) {
  for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
    const Arguments arguments(input, row);
    if (arguments.IsNull(0)) {
      continue;
    }
    Sketch& sketch = *State(states[row]);
    if (!sketch) {
      const int64_t precision = arguments.IsNull(1) ? 0 : arguments.Int(1);
      if (precision < 10 || precision > 24) {
        duckdb_aggregate_function_set_error(
            info, "Argument 2 to HLL_COUNT.INIT must be between 10 and 24");
        return;
      }
      const auto normal = static_cast<int>(precision);
      sketch.emplace(normal,
                     std::min(normal + kDefaultSparsePrecisionDelta,
                              HyperLogLogPlusPlus::kMaximumSparsePrecision),
                     static_cast<int32_t>(kType));
    }
    const auto hash = Hash(kType, arguments.Vector(0), row);
    if (!hash.ok()) {
      duckdb_aggregate_function_set_error(info, std::string(hash.status().message()).c_str());
      return;
    }
    sketch->AddHash(*hash);
  }
}

// HLL_COUNT.MERGE_PARTIAL merges each non-NULL sketch, argument 0.
void MergeSketches(duckdb_function_info info, duckdb_data_chunk input,
                   duckdb_aggregate_state* states) {
  for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
    const Arguments arguments(input, row);
    if (arguments.IsNull(0)) {
      continue;
    }
    const auto sketch = HyperLogLogPlusPlus::Parse(arguments.String(0));
    const char* error = sketch ? MergeInto(*State(states[row]), *sketch) : kInvalidSketch;
    if (error != nullptr) {
      duckdb_aggregate_function_set_error(info, error);
      return;
    }
  }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): DuckDB's callback.
void CombineStates(duckdb_function_info info, duckdb_aggregate_state* source,
                   duckdb_aggregate_state* target, idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    // DuckDB may combine a window's states again, so the source stays as it is.
    const Sketch& from = *State(source[i]);
    if (!from) {
      continue;
    }
    if (const char* error = MergeInto(*State(target[i]), *from); error != nullptr) {
      duckdb_aggregate_function_set_error(info, error);
      return;
    }
  }
}

// The serialized sketch, or NULL for one that no value or sketch reached.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): DuckDB's callback.
void FinalizeStates(duckdb_function_info /*info*/, duckdb_aggregate_state* source,
                    duckdb_vector result, idx_t count, idx_t offset) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  duckdb_vector_ensure_validity_writable(result);
  for (idx_t i = 0; i < count; ++i) {
    Sketch& sketch = *State(source[i]);
    SetResult(result, offset + i,
              sketch ? std::optional<std::string>(sketch->Serialize()) : std::nullopt);
  }
}

// An aggregate function of `parameters` that returns a sketch, adding each row with `update`.
Handle<duckdb_aggregate_function, duckdb_destroy_aggregate_function> SketchAggregate(
    const char* name, const std::vector<duckdb_logical_type>& parameters,
    duckdb_aggregate_update_t update) {
  Handle<duckdb_aggregate_function, duckdb_destroy_aggregate_function> function(
      duckdb_create_aggregate_function());
  duckdb_aggregate_function_set_name(function.get(), name);
  for (duckdb_logical_type parameter : parameters) {
    duckdb_aggregate_function_add_parameter(function.get(), parameter);
  }
  LogicalType const blob(duckdb_create_logical_type(kBlob));
  duckdb_aggregate_function_set_return_type(function.get(), blob.get());
  duckdb_aggregate_function_set_functions(function.get(), StateSize, InitState, update,
                                          CombineStates, FinalizeStates);
  duckdb_aggregate_function_set_destructor(function.get(), DestroyStates);
  // The functions skip NULL values themselves.
  duckdb_aggregate_function_set_special_handling(function.get());
  return function;
}

void RegisterAggregate(duckdb_connection connection, const char* name,
                       duckdb_aggregate_function function) {
  if (duckdb_register_aggregate_function(connection, function) == DuckDBError) {
    throw BackendError(std::string("DuckDB failed to register ") + name);
  }
}

// HLL_COUNT.EXTRACT, which estimates 0 for a NULL sketch.
absl::StatusOr<int64_t> Extract(const Arguments& arguments) {
  if (arguments.IsNull(0)) {
    return 0;
  }
  auto sketch = HyperLogLogPlusPlus::Parse(arguments.String(0));
  if (!sketch) {
    return absl::OutOfRangeError("Invalid input bytes to HLL_COUNT.EXTRACT");
  }
  return sketch->Estimate();
}

}  // namespace

void RegisterHllCountFunctions(duckdb_connection connection) {
  LogicalType const bigint(duckdb_create_logical_type(kBigint));
  LogicalType const varchar(duckdb_create_logical_type(kVarchar));
  LogicalType const blob(duckdb_create_logical_type(kBlob));
  LogicalType const numeric(duckdb_create_decimal_type(38, 9));

  Handle<duckdb_aggregate_function_set, duckdb_destroy_aggregate_function_set> const init(
      duckdb_create_aggregate_function_set("bq_hll_count_init"));
  for (const auto& function : {
           SketchAggregate("bq_hll_count_init", {bigint.get(), bigint.get()},
                           AddValues<ValueType::kInt64>),
           SketchAggregate("bq_hll_count_init", {varchar.get(), bigint.get()},
                           AddValues<ValueType::kBytes>),
           SketchAggregate("bq_hll_count_init", {blob.get(), bigint.get()},
                           AddValues<ValueType::kBytes>),
           SketchAggregate("bq_hll_count_init", {numeric.get(), bigint.get()},
                           AddValues<ValueType::kNumeric>),
       }) {
    if (duckdb_add_aggregate_function_to_set(init.get(), function.get()) == DuckDBError) {
      throw BackendError("DuckDB failed to add an overload of bq_hll_count_init");
    }
  }
  if (duckdb_register_aggregate_function_set(connection, init.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_hll_count_init");
  }
  // A BIGNUMERIC comes as the VARCHAR of its units, which a STRING overload would take.
  RegisterAggregate(connection, "bq_hll_count_init_bignumeric",
                    SketchAggregate("bq_hll_count_init_bignumeric", {varchar.get(), bigint.get()},
                                    AddValues<ValueType::kBigNumeric>)
                        .get());
  RegisterAggregate(
      connection, "bq_hll_count_merge_partial",
      SketchAggregate("bq_hll_count_merge_partial", {blob.get()}, MergeSketches).get());
  Register(connection, "bq_hll_count_extract", {kBlob}, kBigint, EachRow<Extract, false>,
           /*nulls=*/false);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
