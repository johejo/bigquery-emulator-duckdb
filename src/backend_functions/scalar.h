#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
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
    return std::bit_cast<__int128>((static_cast<unsigned __int128>(value.upper) << 64U) |
                                   value.lower);
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

  [[nodiscard]] duckdb_vector Vector(idx_t column) const {
    return duckdb_data_chunk_get_vector(input_, column);
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

inline void SetResult(duckdb_vector output, idx_t row, duckdb_interval value) {
  static_cast<duckdb_interval*>(duckdb_vector_get_data(output))[row] = value;
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
      .lower = static_cast<uint64_t>(value),
      .upper = static_cast<int64_t>(static_cast<unsigned __int128>(value) >> 64U),
  };
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

}  // namespace bigquery_emulator_duckdb::backend_functions
