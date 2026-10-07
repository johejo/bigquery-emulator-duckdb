#pragma once

// Shared by the sources of the backend functions; not part of their interface, which is
// src/backend_functions.h. Each source implements the functions of one area with GoogleSQL's
// implementation and registers them.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/public/civil_time.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::backend_functions {

inline constexpr duckdb_type kAny = DUCKDB_TYPE_ANY;
inline constexpr duckdb_type kBigint = DUCKDB_TYPE_BIGINT;
inline constexpr duckdb_type kBlob = DUCKDB_TYPE_BLOB;
inline constexpr duckdb_type kBoolean = DUCKDB_TYPE_BOOLEAN;
inline constexpr duckdb_type kDate = DUCKDB_TYPE_DATE;
inline constexpr duckdb_type kDatetime = DUCKDB_TYPE_TIMESTAMP;
inline constexpr duckdb_type kDouble = DUCKDB_TYPE_DOUBLE;
inline constexpr duckdb_type kTime = DUCKDB_TYPE_TIME;
inline constexpr duckdb_type kTimestamp = DUCKDB_TYPE_TIMESTAMP_TZ;
inline constexpr duckdb_type kVarchar = DUCKDB_TYPE_VARCHAR;

// The arguments of one row of a DuckDB function call.
class Arguments {
 public:
  Arguments(duckdb_data_chunk input, idx_t row) : input_(input), row_(row) {}

  [[nodiscard]] std::string String(idx_t column) const {
    return VectorString(duckdb_data_chunk_get_vector(input_, column), row_);
  }

  [[nodiscard]] int64_t Int(idx_t column) const {
    return static_cast<int64_t*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(input_, column)))[row_];
  }

  // A DATE as days since the epoch.
  [[nodiscard]] int32_t Date(idx_t column) const {
    return static_cast<int32_t*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(input_, column)))[row_];
  }

  [[nodiscard]] double Double(idx_t column) const {
    return static_cast<double*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(input_, column)))[row_];
  }

  // A DECIMAL(38, s) as its 128-bit integer of units of 10^-s.
  [[nodiscard]] __int128 Decimal(idx_t column) const {
    const auto value = static_cast<duckdb_hugeint*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(input_, column)))[row_];
    return (static_cast<__int128>(value.upper) << 64) | value.lower;
  }

  // A list of VARCHAR without NULL elements.
  [[nodiscard]] std::vector<std::string> Strings(idx_t column) const {
    duckdb_vector vector = duckdb_data_chunk_get_vector(input_, column);
    const auto entry = VectorElement<duckdb_list_entry>(vector, row_);
    duckdb_vector child = duckdb_list_vector_get_child(vector);
    std::vector<std::string> strings;
    strings.reserve(entry.length);
    for (idx_t i = 0; i < entry.length; ++i) {
      strings.push_back(VectorString(child, entry.offset + i));
    }
    return strings;
  }

  [[nodiscard]] bool Bool(idx_t column) const {
    return static_cast<bool*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(input_, column)))[row_];
  }

  [[nodiscard]] idx_t Row() const { return row_; }

  [[nodiscard]] bool IsNull(idx_t column) const {
    uint64_t* validity = duckdb_vector_get_validity(duckdb_data_chunk_get_vector(input_, column));
    return validity != nullptr && !duckdb_validity_row_is_valid(validity, row_);
  }

 private:
  duckdb_data_chunk input_;
  idx_t row_;
};

inline void SetResult(duckdb_vector output, idx_t row, int64_t value) {
  static_cast<int64_t*>(duckdb_vector_get_data(output))[row] = value;
}

// A DATE as days since the epoch.
inline void SetResult(duckdb_vector output, idx_t row, int32_t value) {
  static_cast<int32_t*>(duckdb_vector_get_data(output))[row] = value;
}

inline void SetResult(duckdb_vector output, idx_t row, bool value) {
  static_cast<bool*>(duckdb_vector_get_data(output))[row] = value;
}

inline void SetResult(duckdb_vector output, idx_t row, double value) {
  static_cast<double*>(duckdb_vector_get_data(output))[row] = value;
}

// A DECIMAL(38, s) as its 128-bit integer of units of 10^-s.
inline void SetResult(duckdb_vector output, idx_t row, __int128 value) {
  static_cast<duckdb_hugeint*>(duckdb_vector_get_data(output))[row] = {
      static_cast<uint64_t>(value), static_cast<int64_t>(value >> 64)};
}

inline void SetResult(duckdb_vector output, idx_t row, const std::string& value) {
  duckdb_vector_assign_string_element_len(output, row, value.data(), value.size());
}

template <typename T>
void SetResult(duckdb_vector output, idx_t row, const std::optional<T>& value);

// An array appended to the list's child vector, where a null element is NULL.
template <typename Element>
void SetResult(duckdb_vector output, idx_t row, const std::vector<Element>& values) {
  const idx_t offset = duckdb_list_vector_get_size(output);
  duckdb_list_vector_reserve(output, offset + values.size());
  duckdb_vector child = duckdb_list_vector_get_child(output);
  duckdb_vector_ensure_validity_writable(child);
  for (idx_t i = 0; i < values.size(); ++i) {
    SetResult(child, offset + i, values[i]);
  }
  duckdb_list_vector_set_size(output, offset + values.size());
  static_cast<duckdb_list_entry*>(duckdb_vector_get_data(output))[row] = {offset, values.size()};
}

template <typename T>
void SetResult(duckdb_vector output, idx_t row, const std::optional<T>& value) {
  if (value) {
    SetResult(output, row, *value);
  } else {
    duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
  }
}

// Sets each row of `output` to what `compute` returns for the row's arguments. Unless `nulls`
// is false, a NULL argument makes a NULL result without calling `compute`. An error fails the
// query.
template <typename Compute>
void EachRow(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output,
             Compute compute, bool nulls = true) {
  const idx_t columns = duckdb_data_chunk_get_column_count(input);
  duckdb_vector_ensure_validity_writable(output);
  uint64_t* output_validity = duckdb_vector_get_validity(output);
  for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
    const Arguments arguments(input, row);
    bool null = false;
    for (idx_t column = 0; nulls && column < columns; ++column) {
      null = null || arguments.IsNull(column);
    }
    if (null) {
      duckdb_validity_set_row_invalid(output_validity, row);
      continue;
    }
    const auto result = compute(arguments);
    if (!result.ok()) {
      duckdb_scalar_function_set_error(info, std::string(result.status().message()).c_str());
      return;
    }
    SetResult(output, row, *result);
  }
}

// Turns a GoogleSQL function's out parameter and error into a StatusOr.
template <typename T>
absl::StatusOr<T> ToStatusOr(bool ok, T value, const absl::Status& error) {
  if (!ok) {
    return error;
  }
  return value;
}

// DuckDB keeps a TIME, DATETIME or TIMESTAMP as microseconds since midnight or the epoch, a
// DATETIME as the civil time in UTC.
googlesql::TimeValue TimeFromMicros(int64_t micros);
int64_t MicrosFromTime(const googlesql::TimeValue& time);
absl::StatusOr<googlesql::DatetimeValue> DatetimeFromMicros(int64_t micros);
absl::StatusOr<int64_t> MicrosFromDatetime(const googlesql::DatetimeValue& datetime);

// Registers `function` under `name`, taking `parameters` and returning `result`. Unless `nulls`
// is false, DuckDB makes the result NULL for a NULL argument without calling `function`. A
// `volatile_result` keeps DuckDB from folding or sharing calls with the same arguments.
// Throws BackendError when DuckDB rejects it.
void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_logical_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt, bool volatile_result = false);
void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt, bool volatile_result = false);
void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt, bool volatile_result = false);

// bignumeric.cc: BIGNUMERIC arithmetic and conversions.
void RegisterBigNumericFunctions(duckdb_connection connection);
// math.cc: FLOAT64 and NUMERIC functions, such as SQRT and POW.
void RegisterMathFunctions(duckdb_connection connection);
// string.cc: STRING and BYTES functions, including hashing, regular expressions and NET.
void RegisterStringFunctions(duckdb_connection connection);
// datetime.cc: formatting and parsing dates and times, and FORMAT.
void RegisterDatetimeFunctions(duckdb_connection connection);
// json.cc: JSON functions.
void RegisterJsonFunctions(duckdb_connection connection);
// aead.cc: AEAD encryption with Tink keysets.
void RegisterAeadFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::backend_functions
