#include "src/result_rows.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "duckdb.h"
#include "googlesql/public/numeric_value.h"
#include "nlohmann/json.hpp"
#include "src/backend_error.h"
#include "src/bignumeric.h"
#include "src/duckdb_handle.h"
#include "src/duckdb_types.h"
#include "src/interval.h"
#include "src/result_chunks.h"
#include "src/timestamp_nanos.h"
#include "src/vector_value.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// The bounds of a RANGE<TIMESTAMP> in BigQuery's default encoding: a decimal string of seconds
// since the Unix epoch with all six digits of the fraction, even for a whole second.
std::string EpochSecondsString(int64_t micros) {
  const bool negative = micros < 0;
  const uint64_t magnitude =
      negative ? static_cast<uint64_t>(-(micros + 1)) + 1 : static_cast<uint64_t>(micros);
  std::string digits = std::to_string(magnitude % 1000000);
  digits.insert(0, 6 - digits.size(), '0');
  return (negative ? "-" : "") + std::to_string(magnitude / 1000000) + "." + digits;
}

// BigQuery's default TIMESTAMP encoding: seconds since the Unix epoch as a FLOAT64, written as
// Java's Double.toString writes it, such as 1.0, -0.5 or 1.57793424525E9.
std::string EpochSecondsDouble(int64_t micros) {
  const double seconds = static_cast<double>(micros) / 1e6;
  if (seconds == 0) {
    return "0.0";
  }
  // The shortest digits that read back as `seconds`, as d.ddde[+-]x.
  std::array<char, 32> buffer{};
  const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), seconds,
                                       std::chars_format::scientific);
  const std::string_view text(buffer.data(), converted.ptr);
  const size_t e = text.find('e');
  const int exponent = std::stoi(std::string(text.substr(e + 1)));
  std::string digits;
  std::ranges::copy_if(text.substr(0, e), std::back_inserter(digits),
                       [](char c) { return c >= '0' && c <= '9'; });
  const std::string result = seconds < 0 ? "-" : "";
  if (std::abs(seconds) < 1e-3 || std::abs(seconds) >= 1e7) {
    return result + digits.front() + "." + (digits.size() > 1 ? digits.substr(1) : "0") + "E" +
           std::to_string(exponent);
  }
  if (exponent < 0) {
    return result + "0." + std::string(-exponent - 1, '0') + digits;
  }
  const auto integer_digits = static_cast<size_t>(exponent) + 1;
  if (digits.size() <= integer_digits) {
    return result + digits + std::string(integer_digits - digits.size(), '0') + ".0";
  }
  return result + digits.substr(0, integer_digits) + "." + digits.substr(integer_digits);
}

// BigQuery writes a FLOAT64 outside [1e-3, 1e7) with an exponent, as 2.001511821194711E7, in the
// shortest digits that round-trip, infinities as Infinity and -Infinity, NaN as NaN, and others as
// DuckDB does, such as 20.0 and -0.0.
std::string DoubleText(double number, duckdb_value value) {
  if (std::isnan(number)) {
    return "NaN";
  }
  if (std::isinf(number)) {
    return number < 0 ? "-Infinity" : "Infinity";
  }
  const double magnitude = std::abs(number);
  // BigQuery uses two significant digits for the smallest subnormal, which bounds at zero
  // in ST_BOUNDINGBOX return. to_chars would write 5e-324 instead.
  if (magnitude == std::numeric_limits<double>::denorm_min()) {
    return number < 0 ? "-4.9E-324" : "4.9E-324";
  }
  if (number == 0 || (magnitude >= 1e-3 && magnitude < 1e7)) {
    return ValueString(value);
  }
  std::string text(32, '\0');
  const auto result =
      std::to_chars(text.data(), text.data() + text.size(), number, std::chars_format::scientific);
  text.resize(result.ptr - text.data());
  // to_chars writes 2.001511821194711e+07 and 1e+20.
  const size_t e = text.find('e');
  std::string mantissa = text.substr(0, e);
  if (mantissa.find('.') == std::string::npos) {
    mantissa += ".0";
  }
  const int exponent = std::stoi(text.substr(e + 1));
  return mantissa + "E" + std::to_string(exponent);
}

std::string Base64(const std::string& bytes) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const auto a = static_cast<unsigned char>(bytes.at(i));
    const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes.at(i + 1)) : 0U;
    const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes.at(i + 2)) : 0U;
    result += alphabet[a >> 2U];
    result += alphabet[((a & 3U) << 4U) | (b >> 4U)];
    result += i + 1 < bytes.size() ? alphabet[((b & 15U) << 2U) | (c >> 6U)] : '=';
    result += i + 2 < bytes.size() ? alphabet[c & 63U] : '=';
  }
  return result;
}

// How a cell encodes its value. A NULL array is an empty one, as BigQuery returns it, unless
// `null_arrays` is set.
struct CellEncoding {
  bool null_arrays = false;
  TimestampEncoding timestamps = TimestampEncoding::kMicros;
};

// The cell, {"v": ...}, of `row` of a result vector in BigQuery's wire format.
json ToCell(duckdb_vector vector, duckdb_logical_type type, idx_t row,
            const CellEncoding& encoding);

// A RANGE as BigQuery writes it, "[start, end)", with each bound as its own cell would be, or
// UNBOUNDED for an infinite or NULL one. A TIMESTAMP bound in seconds keeps all six digits of
// the fraction, unlike a TIMESTAMP cell.
std::string RangeText(duckdb_vector vector, duckdb_logical_type type, idx_t row,
                      TimestampEncoding timestamps) {
  std::string text = "[";
  for (idx_t i = 0; i < 2; ++i) {
    duckdb_vector child = duckdb_struct_vector_get_child(vector, i);
    LogicalType const bound(duckdb_struct_type_child_type(type, i));
    text += i == 0 ? "" : ", ";
    if (!IsFiniteBound(child, bound.get(), row)) {
      text += "UNBOUNDED";
    } else if (duckdb_get_type_id(bound.get()) == DUCKDB_TYPE_TIMESTAMP_TZ &&
               timestamps == TimestampEncoding::kSeconds) {
      text += EpochSecondsString(VectorElement<duckdb_timestamp>(child, row).micros);
    } else {
      text += ToCell(child, bound.get(), row, {}).at("v").get<std::string>();
    }
  }
  return text + ")";
}

json ToCell(duckdb_vector vector, duckdb_logical_type type, idx_t row,
            const CellEncoding& encoding) {
  const bool is_null = IsNull(vector, row);
  if (IsListLike(type) && !(is_null && encoding.null_arrays)) {
    json elements = json::array();
    if (!is_null) {
      const ListElements list(vector, type, row);
      for (idx_t i = 0; i < list.size(); ++i) {
        elements.push_back(ToCell(list.child(), list.type(), list.row(i), encoding));
      }
    }
    return json{{"v", std::move(elements)}};
  }
  if (is_null) {
    return json{{"v", nullptr}};
  }
  if (IsRange(type)) {
    return json{{"v", RangeText(vector, type, row, encoding.timestamps)}};
  }
  if (IsTimestampNanos(type)) {
    const auto text =
        TimestampNanosText(TimestampNanosInteger(VectorElement<duckdb_hugeint>(vector, row)));
    if (!text.ok()) {
      throw BackendError(std::string(text.status().message()));
    }
    return json{{"v", *text}};
  }
  json value;
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_STRUCT: {
      json fields = json::array();
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        LogicalType const child_type(duckdb_struct_type_child_type(type, i));
        fields.push_back(
            ToCell(duckdb_struct_vector_get_child(vector, i), child_type.get(), row, encoding));
      }
      value = json{{"f", std::move(fields)}};
      break;
    }
    case DUCKDB_TYPE_VARCHAR:
      value = VectorString(vector, row);
      break;
    case DUCKDB_TYPE_BLOB:
      value = Base64(VectorString(vector, row));
      break;
    case DUCKDB_TYPE_DOUBLE:
      value = DoubleText(VectorElement<double>(vector, row), VectorValue(vector, type, row).get());
      break;
    case DUCKDB_TYPE_TIMESTAMP_TZ: {
      const int64_t micros = VectorElement<duckdb_timestamp>(vector, row).micros;
      value = encoding.timestamps == TimestampEncoding::kSeconds ? EpochSecondsDouble(micros)
                                                                 : std::to_string(micros);
      break;
    }
    case DUCKDB_TYPE_TIMESTAMP:
    case DUCKDB_TYPE_TIMESTAMP_S:
    case DUCKDB_TYPE_TIMESTAMP_MS:
    case DUCKDB_TYPE_TIMESTAMP_NS: {
      std::string text;
      if (duckdb_get_type_id(type) == DUCKDB_TYPE_TIMESTAMP_NS) {
        // Match a cast to microsecond TIMESTAMP, including truncation before the epoch.
        const int64_t nanos = VectorElement<duckdb_timestamp_ns>(vector, row).nanos;
        const bool infinite = nanos == std::numeric_limits<int64_t>::max() ||
                              nanos == -std::numeric_limits<int64_t>::max();
        Value const timestamp(duckdb_create_timestamp({infinite ? nanos : nanos / 1000}));
        text = ValueString(timestamp.get());
      } else {
        text = ValueString(VectorValue(vector, type, row).get());
      }
      if (text.size() > 10 && text.at(10) == ' ') {
        text.at(10) = 'T';
      }
      value = std::move(text);
      break;
    }
    case DUCKDB_TYPE_INTERVAL: {
      const auto interval = IntervalFromDuckDb(VectorElement<duckdb_interval>(vector, row));
      if (!interval.ok()) {
        throw BackendError("DuckDB returned an invalid INTERVAL: " +
                           std::string(interval.status().message()));
      }
      value = interval->ToString();
      break;
    }
    case DUCKDB_TYPE_BIGNUM: {
      const auto number = BigNumericFromUnits(ValueString(VectorValue(vector, type, row).get()));
      if (!number.ok()) {
        throw BackendError("DuckDB returned an invalid BIGNUMERIC: " +
                           std::string(number.status().message()));
      }
      value = number->ToString();
      break;
    }
    case DUCKDB_TYPE_DECIMAL: {
      // DuckDB pads a DECIMAL with zeros to its scale, which BigQuery leaves out. Every DECIMAL
      // fits a BIGNUMERIC.
      const auto number =
          googlesql::BigNumericValue::FromString(ValueString(VectorValue(vector, type, row).get()));
      if (!number.ok()) {
        throw BackendError("DuckDB returned an invalid DECIMAL: " +
                           std::string(number.status().message()));
      }
      value = number->ToString();
      break;
    }
    default:
      value = ValueString(VectorValue(vector, type, row).get());
  }
  return json{{"v", std::move(value)}};
}

// Whether encoding a value of `type` as a cell can fail, such as a BIGNUMERIC out of range.
bool MayFailToEncode(duckdb_logical_type type) {
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BOOLEAN:
    case DUCKDB_TYPE_TINYINT:
    case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_INTEGER:
    case DUCKDB_TYPE_BIGINT:
    case DUCKDB_TYPE_UTINYINT:
    case DUCKDB_TYPE_USMALLINT:
    case DUCKDB_TYPE_UINTEGER:
    case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UHUGEINT:
    case DUCKDB_TYPE_FLOAT:
    case DUCKDB_TYPE_DOUBLE:
    case DUCKDB_TYPE_DATE:
    case DUCKDB_TYPE_TIME:
    case DUCKDB_TYPE_TIME_NS:
    case DUCKDB_TYPE_TIME_TZ:
    case DUCKDB_TYPE_TIMESTAMP:
    case DUCKDB_TYPE_TIMESTAMP_S:
    case DUCKDB_TYPE_TIMESTAMP_MS:
    case DUCKDB_TYPE_TIMESTAMP_NS:
    case DUCKDB_TYPE_TIMESTAMP_TZ:
    case DUCKDB_TYPE_VARCHAR:
    case DUCKDB_TYPE_BLOB:
    case DUCKDB_TYPE_UUID:
    case DUCKDB_TYPE_ENUM:
    case DUCKDB_TYPE_BIT:
      return false;
    case DUCKDB_TYPE_LIST:
    case DUCKDB_TYPE_ARRAY:
      return MayFailToEncode(ElementType(type).get());
    case DUCKDB_TYPE_STRUCT:
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        LogicalType const child(duckdb_struct_type_child_type(type, i));
        if (MayFailToEncode(child.get())) {
          return true;
        }
      }
      return false;
    default:
      return true;
  }
}

}  // namespace

ResultRows::ResultRows(std::shared_ptr<const ResultChunks> chunks, bool null_arrays)
    : chunks_(std::move(chunks)), null_arrays_(null_arrays) {
  for (size_t column = 0; column < chunks_->column_count(); ++column) {
    if (!MayFailToEncode(chunks_->type(column))) {
      continue;
    }
    chunks_->Runs(0, chunks_->row_count(),
                  [&](duckdb_data_chunk chunk, size_t begin, size_t take, size_t) {
                    for (size_t row = begin; row < begin + take; ++row) {
                      (void)ToCell(duckdb_data_chunk_get_vector(chunk, column),
                                   chunks_->type(column), row, {.null_arrays = null_arrays_});
                    }
                  });
  }
}

size_t ResultRows::size() const { return chunks_ ? chunks_->row_count() : 0; }

json ResultRows::at(size_t row, TimestampEncoding timestamps) const {
  if (row >= size()) {
    throw std::out_of_range("Query result row is out of range");
  }
  json cells = json::array();
  chunks_->Runs(row, 1, [&](duckdb_data_chunk chunk, size_t index, size_t, size_t) {
    for (size_t column = 0; column < chunks_->column_count(); ++column) {
      cells.push_back(ToCell(duckdb_data_chunk_get_vector(chunk, column), chunks_->type(column),
                             index, {.null_arrays = null_arrays_, .timestamps = timestamps}));
    }
  });
  return json{{"f", std::move(cells)}};
}

namespace {

// The value of `column` in row `row` of `chunks`, as `read` reads it from the column's vector, or
// nullopt for NULL. The column's type must be `type`.
template <typename T, typename F>
std::optional<T> ReadValue(const ResultChunks& chunks, size_t row, size_t column, duckdb_type type,
                           const F& read) {
  if (row >= chunks.row_count() || column >= chunks.column_count()) {
    throw std::out_of_range("Query result value is out of range");
  }
  if (duckdb_get_type_id(chunks.type(column)) != type) {
    throw BackendError("Query result column " + std::to_string(column) + " has DuckDB type " +
                       std::to_string(duckdb_get_type_id(chunks.type(column))) + ", not " +
                       std::to_string(type));
  }
  std::optional<T> value;
  chunks.Runs(row, 1, [&](duckdb_data_chunk chunk, size_t index, size_t, size_t) {
    duckdb_vector vector = duckdb_data_chunk_get_vector(chunk, column);
    if (!IsNull(vector, index)) {
      value = read(vector, index);
    }
  });
  return value;
}

// The value of `column` in row `row`, which must not be NULL.
template <typename T>
T NotNull(std::optional<T> value, size_t row, size_t column) {
  if (!value.has_value()) {
    throw BackendError("Query result column " + std::to_string(column) + " is NULL in row " +
                       std::to_string(row));
  }
  return *std::move(value);
}

}  // namespace

std::optional<std::string> ResultRows::OptionalString(size_t row, size_t column) const {
  if (!chunks_) {
    throw std::out_of_range("Query result value is out of range");
  }
  return ReadValue<std::string>(*chunks_, row, column, DUCKDB_TYPE_VARCHAR, VectorString);
}

std::string ResultRows::String(size_t row, size_t column) const {
  return NotNull(OptionalString(row, column), row, column);
}

int64_t ResultRows::Int64(size_t row, size_t column) const {
  if (!chunks_) {
    throw std::out_of_range("Query result value is out of range");
  }
  return NotNull(
      ReadValue<int64_t>(*chunks_, row, column, DUCKDB_TYPE_BIGINT, VectorElement<int64_t>), row,
      column);
}

}  // namespace bigquery_emulator_duckdb
