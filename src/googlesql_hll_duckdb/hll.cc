#include "src/googlesql_hll_duckdb/hll.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "src/backend_error.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/bignumeric.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_hll/hll.h"
#include "src/zetasketch/hyperloglog_plus_plus.h"

namespace bigquery_emulator_duckdb::hll_duckdb {
namespace {

using namespace backend_functions;
using googlesql_hll::Sketch;
using googlesql_hll::ValueType;

// The hash of row `row` of `vector`, a value of `type`: Fingerprint2011 of its bytes, of an INT64's
// 8 little-endian bytes, and of the little-endian two's complement of a NUMERIC's or BIGNUMERIC's
// units, in 16 or 32 bytes.
absl::StatusOr<uint64_t> Hash(ValueType type, duckdb_vector vector, idx_t row) {
  switch (type) {
    case ValueType::kInt64:
      return googlesql_hll::HashInt64(VectorElement<int64_t>(vector, row));
    case ValueType::kBytes:
      return googlesql_hll::HashBytes(VectorString(vector, row));
    case ValueType::kNumeric: {
      const auto value = VectorElement<duckdb_hugeint>(vector, row);
      return googlesql_hll::HashNumeric(value.lower, value.upper);
    }
    case ValueType::kBigNumeric: {
      const auto value = BigNumericFromUnits(VectorString(vector, row));
      if (!value.ok()) {
        return value.status();
      }
      return googlesql_hll::HashBigNumeric(BigNumericDecimalBytes(*value));
    }
  }
  return 0;
}

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

// HLL_COUNT.INIT adds each non-NULL value, argument 0, to a sketch of the precision in argument
// 1, which the first value sets.
template <ValueType kType>
void AddValues(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state* states) {
  for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
    const Arguments arguments(input, row);
    if (arguments.IsNull(0)) {
      continue;
    }
    const auto sketch = googlesql_hll::Initialize(*State(states[row]), kType,
                                                  arguments.IsNull(1) ? 0 : arguments.Int(1));
    if (!sketch.ok()) {
      duckdb_aggregate_function_set_error(info, std::string(sketch.status().message()).c_str());
      return;
    }
    const auto hash = Hash(kType, arguments.Vector(0), row);
    if (!hash.ok()) {
      duckdb_aggregate_function_set_error(info, std::string(hash.status().message()).c_str());
      return;
    }
    (*sketch)->AddHash(*hash);
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
    const char* error = googlesql_hll::MergeSerialized(*State(states[row]), arguments.String(0));
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
    if (const char* error = googlesql_hll::MergeInto(*State(target[i]), *from); error != nullptr) {
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
    return googlesql_hll::Extract(std::nullopt);
  }
  return googlesql_hll::Extract(arguments.String(0));
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

}  // namespace bigquery_emulator_duckdb::hll_duckdb
