#include "src/backend.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "duckdb.h"
#include "googlesql/public/numeric_value.h"
#include "nlohmann/json.hpp"
#include "src/backend_error.h"
#include "src/backend_functions.h"
#include "src/backend_functions/javascript_limits.h"
#include "src/bignumeric.h"
#include "src/duckdb_handle.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/interval.h"
#include "src/postgres_scanner.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

struct Result {
  duckdb_result result{};
  ~Result() { duckdb_destroy_result(&result); }
  Result() = default;
  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;
};

LogicalType ElementType(duckdb_logical_type type) {
  return LogicalType(duckdb_get_type_id(type) == DUCKDB_TYPE_ARRAY
                         ? duckdb_array_type_child_type(type)
                         : duckdb_list_type_child_type(type));
}

FieldSchema ToFieldSchema(const std::string& name, duckdb_logical_type type);

// Maps a DuckDB type to the BigQuery type of a TableFieldSchema.
FieldType ToBigQueryType(duckdb_logical_type type) {
  if (IsRange(type)) {
    return FieldType::kRange;
  }
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BOOLEAN:
      return FieldType::kBoolean;
    case DUCKDB_TYPE_TINYINT:
    case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_INTEGER:
    case DUCKDB_TYPE_BIGINT:
    case DUCKDB_TYPE_UTINYINT:
    case DUCKDB_TYPE_USMALLINT:
    case DUCKDB_TYPE_UINTEGER:
      return FieldType::kInteger;
    case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UHUGEINT:
    case DUCKDB_TYPE_BIGNUM:
      return FieldType::kBigNumeric;
    case DUCKDB_TYPE_FLOAT:
    case DUCKDB_TYPE_DOUBLE:
      return FieldType::kFloat;
    case DUCKDB_TYPE_DECIMAL:
      return duckdb_decimal_width(type) - duckdb_decimal_scale(type) <= 29 &&
                     duckdb_decimal_scale(type) <= 9
                 ? FieldType::kNumeric
                 : FieldType::kBigNumeric;
    case DUCKDB_TYPE_VARCHAR:
    case DUCKDB_TYPE_UUID:
      return FieldType::kString;
    case DUCKDB_TYPE_BLOB:
      return FieldType::kBytes;
    case DUCKDB_TYPE_DATE:
      return FieldType::kDate;
    case DUCKDB_TYPE_TIME:
    case DUCKDB_TYPE_TIME_TZ:
      return FieldType::kTime;
    // BigQuery TIMESTAMP is an absolute instant, which is DuckDB's TIMESTAMP WITH TIME ZONE.
    // BigQuery DATETIME is a civil time, which is DuckDB's plain TIMESTAMP.
    case DUCKDB_TYPE_TIMESTAMP_TZ:
      return FieldType::kTimestamp;
    case DUCKDB_TYPE_TIMESTAMP:
    case DUCKDB_TYPE_TIMESTAMP_S:
    case DUCKDB_TYPE_TIMESTAMP_MS:
    case DUCKDB_TYPE_TIMESTAMP_NS:
      return FieldType::kDatetime;
    case DUCKDB_TYPE_INTERVAL:
      return FieldType::kInterval;
    case DUCKDB_TYPE_STRUCT:
      return FieldType::kRecord;
    case DUCKDB_TYPE_LIST:
    case DUCKDB_TYPE_ARRAY:
      return ToBigQueryType(ElementType(type).get());
    default:
      return FieldType::kString;
  }
}

bool IsListLike(duckdb_logical_type type) {
  return duckdb_get_type_id(type) == DUCKDB_TYPE_LIST ||
         duckdb_get_type_id(type) == DUCKDB_TYPE_ARRAY;
}

// Only marked BIGNUM columns contain units of 10^-38. A plain BIGNUM is an integer,
// with a range larger than BIGNUMERIC's, and is not a supported storage type.
std::string UnsupportedType(duckdb_logical_type type) {
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BIGNUM: {
      DuckString const alias(duckdb_logical_type_get_alias(type));
      return alias.get() != nullptr && std::string_view(alias.get()).starts_with("bq_bignumeric_")
                 ? ""
                 : "BIGNUM";
    }
    case DUCKDB_TYPE_LIST:
    case DUCKDB_TYPE_ARRAY:
      return UnsupportedType(ElementType(type).get());
    case DUCKDB_TYPE_STRUCT:
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        LogicalType const child(duckdb_struct_type_child_type(type, i));
        if (std::string child_type = UnsupportedType(child.get()); !child_type.empty()) {
          return child_type;
        }
      }
      return "";
    default:
      return "";
  }
}

// Converts external numeric storage to the units used by all query expressions. Arrays and
// structs preserve NULLs, including a NULL struct rather than a struct of NULL fields.
std::optional<std::string> ReadNumericStorage(const std::string& sql, duckdb_logical_type type,
                                              int depth) {
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UHUGEINT:
      return "CAST(bq_bignumeric_from_string(CAST(" + sql + " AS VARCHAR), false) AS BIGNUM)";
    case DUCKDB_TYPE_DECIMAL:
      if (ToBigQueryType(type) == FieldType::kBigNumeric) {
        return "CAST(bq_bignumeric_from_string(CAST(" + sql + " AS VARCHAR), false) AS BIGNUM)";
      }
      return std::nullopt;
    case DUCKDB_TYPE_LIST:
    case DUCKDB_TYPE_ARRAY: {
      const std::string element = "_bq_numeric_" + std::to_string(depth);
      const auto converted = ReadNumericStorage(element, ElementType(type).get(), depth + 1);
      if (!converted) {
        return std::nullopt;
      }
      return "list_transform(" + sql + ", " + element + " -> " + *converted + ")";
    }
    case DUCKDB_TYPE_STRUCT: {
      std::vector<std::string> fields;
      bool changed = false;
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        DuckString const name(duckdb_struct_type_child_name(type, i));
        LogicalType const child(duckdb_struct_type_child_type(type, i));
        const std::string field = QuoteIdentifier(name.get());
        const std::string access = std::format("({}).{}", sql, field);
        const auto converted = ReadNumericStorage(access, child.get(), depth + 1);
        changed |= converted.has_value();
        fields.push_back(field + " := " + converted.value_or(access));
      }
      if (!changed) {
        return std::nullopt;
      }
      return "CASE WHEN " + sql + " IS NULL THEN NULL ELSE struct_pack(" + Join(fields, ", ") +
             ") END";
    }
    default:
      return std::nullopt;
  }
}

FieldSchema ToFieldSchema(const std::string& name, duckdb_logical_type type) {
  FieldSchema field;
  field.name = name;
  field.type = ToBigQueryType(type);
  field.mode = IsListLike(type) ? FieldMode::kRepeated : FieldMode::kNullable;
  LogicalType const element(IsListLike(type) ? ElementType(type).release() : nullptr);
  duckdb_logical_type scalar_type = element.get() != nullptr ? element.get() : type;
  if (IsRange(scalar_type)) {
    LogicalType const bound(duckdb_struct_type_child_type(scalar_type, 0));
    field.range_element_type = ToBigQueryType(bound.get());
  } else if (duckdb_get_type_id(scalar_type) == DUCKDB_TYPE_STRUCT) {
    for (idx_t i = 0; i < duckdb_struct_type_child_count(scalar_type); ++i) {
      DuckString const child_name(duckdb_struct_type_child_name(scalar_type, i));
      LogicalType const child_type(duckdb_struct_type_child_type(scalar_type, i));
      field.fields.push_back(ToFieldSchema(child_name.get(), child_type.get()));
    }
  }
  return field;
}

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

std::string ValueString(duckdb_value value) {
  DuckString const text(duckdb_get_varchar(value));
  if (!text) {
    throw BackendError("DuckDB failed to format a value");
  }
  return text.get();
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

// Reconstruct values through the C API so DuckDB formats scalar values itself.
Value VectorValue(duckdb_vector vector, duckdb_logical_type type, idx_t row) {
  uint64_t* validity = duckdb_vector_get_validity(vector);
  if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
    return Value(duckdb_create_null_value());
  }
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BOOLEAN:
      return Value(duckdb_create_bool(VectorElement<bool>(vector, row)));
    case DUCKDB_TYPE_TINYINT:
      return Value(duckdb_create_int8(VectorElement<int8_t>(vector, row)));
    case DUCKDB_TYPE_SMALLINT:
      return Value(duckdb_create_int16(VectorElement<int16_t>(vector, row)));
    case DUCKDB_TYPE_INTEGER:
      return Value(duckdb_create_int32(VectorElement<int32_t>(vector, row)));
    case DUCKDB_TYPE_BIGINT:
      return Value(duckdb_create_int64(VectorElement<int64_t>(vector, row)));
    case DUCKDB_TYPE_UTINYINT:
      return Value(duckdb_create_uint8(VectorElement<uint8_t>(vector, row)));
    case DUCKDB_TYPE_USMALLINT:
      return Value(duckdb_create_uint16(VectorElement<uint16_t>(vector, row)));
    case DUCKDB_TYPE_UINTEGER:
      return Value(duckdb_create_uint32(VectorElement<uint32_t>(vector, row)));
    case DUCKDB_TYPE_UBIGINT:
      return Value(duckdb_create_uint64(VectorElement<uint64_t>(vector, row)));
    case DUCKDB_TYPE_HUGEINT:
      return Value(duckdb_create_hugeint(VectorElement<duckdb_hugeint>(vector, row)));
    case DUCKDB_TYPE_UHUGEINT:
      return Value(duckdb_create_uhugeint(VectorElement<duckdb_uhugeint>(vector, row)));
    case DUCKDB_TYPE_FLOAT:
      return Value(duckdb_create_float(VectorElement<float>(vector, row)));
    case DUCKDB_TYPE_DOUBLE:
      return Value(duckdb_create_double(VectorElement<double>(vector, row)));
    case DUCKDB_TYPE_DATE:
      return Value(duckdb_create_date(VectorElement<duckdb_date>(vector, row)));
    case DUCKDB_TYPE_TIME:
      return Value(duckdb_create_time(VectorElement<duckdb_time>(vector, row)));
    case DUCKDB_TYPE_TIME_NS:
      return Value(duckdb_create_time_ns(VectorElement<duckdb_time_ns>(vector, row)));
    case DUCKDB_TYPE_TIME_TZ:
      return Value(duckdb_create_time_tz_value(VectorElement<duckdb_time_tz>(vector, row)));
    case DUCKDB_TYPE_TIMESTAMP:
      return Value(duckdb_create_timestamp(VectorElement<duckdb_timestamp>(vector, row)));
    case DUCKDB_TYPE_TIMESTAMP_S:
      return Value(duckdb_create_timestamp_s(VectorElement<duckdb_timestamp_s>(vector, row)));
    case DUCKDB_TYPE_TIMESTAMP_MS:
      return Value(duckdb_create_timestamp_ms(VectorElement<duckdb_timestamp_ms>(vector, row)));
    case DUCKDB_TYPE_TIMESTAMP_NS:
      return Value(duckdb_create_timestamp_ns(VectorElement<duckdb_timestamp_ns>(vector, row)));
    case DUCKDB_TYPE_INTERVAL:
      return Value(duckdb_create_interval(VectorElement<duckdb_interval>(vector, row)));
    case DUCKDB_TYPE_DECIMAL: {
      duckdb_hugeint number{};
      switch (duckdb_decimal_internal_type(type)) {
        case DUCKDB_TYPE_SMALLINT: {
          const int64_t value = VectorElement<int16_t>(vector, row);
          number = {.lower = static_cast<uint64_t>(value), .upper = value < 0 ? -1 : 0};
          break;
        }
        case DUCKDB_TYPE_INTEGER: {
          const int64_t value = VectorElement<int32_t>(vector, row);
          number = {.lower = static_cast<uint64_t>(value), .upper = value < 0 ? -1 : 0};
          break;
        }
        case DUCKDB_TYPE_BIGINT: {
          const auto value = VectorElement<int64_t>(vector, row);
          number = {.lower = static_cast<uint64_t>(value), .upper = value < 0 ? -1 : 0};
          break;
        }
        default:
          number = VectorElement<duckdb_hugeint>(vector, row);
      }
      return Value(duckdb_create_decimal({
          .width = duckdb_decimal_width(type),
          .scale = duckdb_decimal_scale(type),
          .value = number,
      }));
    }
    case DUCKDB_TYPE_ENUM: {
      uint64_t index = 0;
      switch (duckdb_enum_internal_type(type)) {
        case DUCKDB_TYPE_UTINYINT:
          index = VectorElement<uint8_t>(vector, row);
          break;
        case DUCKDB_TYPE_USMALLINT:
          index = VectorElement<uint16_t>(vector, row);
          break;
        default:
          index = VectorElement<uint32_t>(vector, row);
      }
      return Value(duckdb_create_enum_value(type, index));
    }
    case DUCKDB_TYPE_UUID: {
      const auto uuid = VectorElement<duckdb_hugeint>(vector, row);
      return Value(duckdb_create_uuid({
          .lower = uuid.lower,
          .upper = static_cast<uint64_t>(uuid.upper) ^ (uint64_t{1} << 63U),
      }));
    }
    case DUCKDB_TYPE_VARCHAR: {
      const std::string text = VectorString(vector, row);
      return Value(duckdb_create_varchar_length(text.data(), text.size()));
    }
    case DUCKDB_TYPE_BLOB: {
      const std::string bytes = VectorString(vector, row);
      return Value(
          duckdb_create_blob(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    }
    case DUCKDB_TYPE_TIMESTAMP_TZ:
      return Value(duckdb_create_timestamp_tz(VectorElement<duckdb_timestamp>(vector, row)));
    case DUCKDB_TYPE_LIST:
    case DUCKDB_TYPE_ARRAY: {
      const bool array = duckdb_get_type_id(type) == DUCKDB_TYPE_ARRAY;
      const idx_t size = array ? duckdb_array_type_array_size(type) : 0;
      const duckdb_list_entry entry = array
                                          ? duckdb_list_entry{.offset = row * size, .length = size}
                                          : VectorElement<duckdb_list_entry>(vector, row);
      duckdb_vector child =
          array ? duckdb_array_vector_get_child(vector) : duckdb_list_vector_get_child(vector);
      LogicalType const child_type = ElementType(type);
      std::vector<Value> values;
      std::vector<duckdb_value> handles;
      for (idx_t i = 0; i < entry.length; ++i) {
        values.push_back(VectorValue(child, child_type.get(), entry.offset + i));
        handles.push_back(values.back().get());
      }
      return Value(
          array ? duckdb_create_array_value(child_type.get(), handles.data(), handles.size())
                : duckdb_create_list_value(child_type.get(), handles.data(), handles.size()));
    }
    case DUCKDB_TYPE_STRUCT: {
      std::vector<Value> values;
      std::vector<duckdb_value> handles;
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        LogicalType const child_type(duckdb_struct_type_child_type(type, i));
        values.push_back(
            VectorValue(duckdb_struct_vector_get_child(vector, i), child_type.get(), row));
        handles.push_back(values.back().get());
      }
      return Value(duckdb_create_struct_value(type, handles.data()));
    }
    case DUCKDB_TYPE_MAP: {
      const auto entry = VectorElement<duckdb_list_entry>(vector, row);
      duckdb_vector entries = duckdb_list_vector_get_child(vector);
      LogicalType const key_type(duckdb_map_type_key_type(type));
      LogicalType const value_type(duckdb_map_type_value_type(type));
      std::vector<Value> values;
      std::vector<duckdb_value> keys;
      std::vector<duckdb_value> mapped;
      for (idx_t i = 0; i < entry.length; ++i) {
        values.push_back(VectorValue(duckdb_struct_vector_get_child(entries, 0), key_type.get(),
                                     entry.offset + i));
        keys.push_back(values.back().get());
        values.push_back(VectorValue(duckdb_struct_vector_get_child(entries, 1), value_type.get(),
                                     entry.offset + i));
        mapped.push_back(values.back().get());
      }
      return Value(duckdb_create_map_value(type, keys.data(), mapped.data(), entry.length));
    }
    case DUCKDB_TYPE_UNION: {
      const auto tag = VectorElement<uint8_t>(duckdb_struct_vector_get_child(vector, 0), row);
      LogicalType const member_type(duckdb_union_type_member_type(type, tag));
      Value const member =
          VectorValue(duckdb_struct_vector_get_child(vector, tag + 1), member_type.get(), row);
      return Value(duckdb_create_union_value(type, tag, member.get()));
    }
    case DUCKDB_TYPE_BIGNUM: {
      // The C API has no BIGNUM vector accessor. DuckDB stores a BIGNUM as a three byte header,
      // whose top bit is set for a value that is not negative, then the big endian bytes of its
      // absolute value, with every bit inverted for a negative value.
      std::string bytes = VectorString(vector, row);
      if (bytes.size() < 3) {
        throw BackendError("DuckDB returned an invalid BIGNUM");
      }
      const bool negative = (static_cast<unsigned char>(bytes.at(0)) & 0x80U) == 0;
      bytes.erase(0, 3);
      if (negative) {
        std::ranges::transform(bytes, bytes.begin(), [](char byte) {
          return static_cast<char>(~static_cast<unsigned char>(byte));
        });
      }
      return Value(duckdb_create_bignum({
          .data = reinterpret_cast<uint8_t*>(bytes.data()),
          .size = bytes.size(),
          .is_negative = negative,
      }));
    }
    case DUCKDB_TYPE_BIT: {
      std::string bytes = VectorString(vector, row);
      return Value(duckdb_create_bit(
          {.data = reinterpret_cast<uint8_t*>(bytes.data()), .size = bytes.size()}));
    }
    default:
      throw BackendError("Unsupported DuckDB result type: " +
                         std::to_string(duckdb_get_type_id(type)));
  }
}

json ToCell(duckdb_vector vector, duckdb_logical_type type, idx_t row, bool null_arrays);

// A RANGE as BigQuery writes it, "[start, end)", with each bound as its own cell would be, or
// UNBOUNDED for an infinite or NULL one.
std::string RangeText(duckdb_vector vector, duckdb_logical_type type, idx_t row) {
  std::string text = "[";
  for (idx_t i = 0; i < 2; ++i) {
    duckdb_vector child = duckdb_struct_vector_get_child(vector, i);
    LogicalType const bound(duckdb_struct_type_child_type(type, i));
    const json cell = ToCell(child, bound.get(), row, false).at("v");
    const bool finite =
        !cell.is_null() &&
        (duckdb_get_type_id(bound.get()) == DUCKDB_TYPE_DATE
             ? duckdb_is_finite_date(VectorElement<duckdb_date>(child, row))
             : duckdb_is_finite_timestamp(VectorElement<duckdb_timestamp>(child, row)));
    text += i == 0 ? "" : ", ";
    text += finite ? cell.get<std::string>() : "UNBOUNDED";
  }
  return text + ")";
}

json ToCell(duckdb_vector vector, duckdb_logical_type type, idx_t row, bool null_arrays) {
  uint64_t* validity = duckdb_vector_get_validity(vector);
  const bool is_null = validity != nullptr && !duckdb_validity_row_is_valid(validity, row);
  if (IsListLike(type) && !(is_null && null_arrays)) {
    json elements = json::array();
    if (!is_null) {
      LogicalType const child_type = ElementType(type);
      const bool array = duckdb_get_type_id(type) == DUCKDB_TYPE_ARRAY;
      const idx_t size = array ? duckdb_array_type_array_size(type) : 0;
      const duckdb_list_entry entry = array
                                          ? duckdb_list_entry{.offset = row * size, .length = size}
                                          : VectorElement<duckdb_list_entry>(vector, row);
      duckdb_vector child =
          array ? duckdb_array_vector_get_child(vector) : duckdb_list_vector_get_child(vector);
      for (idx_t i = 0; i < entry.length; ++i) {
        elements.push_back(ToCell(child, child_type.get(), entry.offset + i, null_arrays));
      }
    }
    return json{{"v", std::move(elements)}};
  }
  if (is_null) {
    return json{{"v", nullptr}};
  }
  if (IsRange(type)) {
    return json{{"v", RangeText(vector, type, row)}};
  }
  json value;
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_STRUCT: {
      json fields = json::array();
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        LogicalType const child_type(duckdb_struct_type_child_type(type, i));
        fields.push_back(
            ToCell(duckdb_struct_vector_get_child(vector, i), child_type.get(), row, null_arrays));
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
    case DUCKDB_TYPE_TIMESTAMP_TZ:
      // Keep epoch microseconds internally for formatOptions.useInt64Timestamp.
      value = std::to_string(VectorElement<duckdb_timestamp>(vector, row).micros);
      break;
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

// Statements that produce a result set. Everything else (DDL, DML, SET, ...) yields a
// single "Count" or "Success" column that is not part of the BigQuery result.
bool ProducesResultSet(duckdb_statement_type type) {
  return type == DUCKDB_STATEMENT_TYPE_SELECT || type == DUCKDB_STATEMENT_TYPE_EXPLAIN ||
         type == DUCKDB_STATEMENT_TYPE_PRAGMA || type == DUCKDB_STATEMENT_TYPE_CALL ||
         type == DUCKDB_STATEMENT_TYPE_EXECUTE;
}

bool IsDml(duckdb_statement_type type) {
  return type == DUCKDB_STATEMENT_TYPE_INSERT || type == DUCKDB_STATEMENT_TYPE_UPDATE ||
         type == DUCKDB_STATEMENT_TYPE_DELETE || type == DUCKDB_STATEMENT_TYPE_MERGE_INTO;
}

json CellsAsSeconds(const std::vector<FieldSchema>& schema, const json& cells);

// One value of a cell: the element of a REPEATED field, or the whole value of a scalar one.
json ValueAsSeconds(const FieldSchema& field, const json& value) {
  if (value.is_null()) {
    return value;
  }
  if (field.type == FieldType::kTimestamp) {
    return EpochSecondsDouble(std::stoll(value.get<std::string>()));
  }
  if (field.type == FieldType::kRange && field.range_element_type == FieldType::kTimestamp) {
    const auto bounds = RangeCellBounds(value.get<std::string>());
    if (!bounds) {
      return value;
    }
    const auto bound = [](const std::optional<std::string>& micros) {
      return micros ? EpochSecondsString(std::stoll(*micros)) : std::string("UNBOUNDED");
    };
    return "[" + bound(bounds->at(0)) + ", " + bound(bounds->at(1)) + ")";
  }
  if (field.type == FieldType::kRecord) {
    return json{{"f", CellsAsSeconds(field.fields, value.at("f"))}};
  }
  return value;
}

json CellsAsSeconds(const std::vector<FieldSchema>& schema, const json& cells) {
  json result = json::array();
  for (size_t i = 0; i < schema.size() && i < cells.size(); ++i) {
    const FieldSchema& field = schema.at(i);
    const json& value = cells[i].at("v");
    if (field.mode != FieldMode::kRepeated || value.is_null()) {
      result.push_back(json{{"v", ValueAsSeconds(field, value)}});
      continue;
    }
    json elements = json::array();
    for (const json& element : value) {
      elements.push_back(json{{"v", ValueAsSeconds(field, element.at("v"))}});
    }
    result.push_back(json{{"v", std::move(elements)}});
  }
  return result;
}

// DuckDB's message for a failed statement. DuckDB's error() raises the messages the translation
// writes as BigQuery's, prefixed with the class of exception it raises, which is left out.
std::string ErrorMessage(const char* error, std::string_view fallback) {
  constexpr std::string_view kInvalidInput = "Invalid Input Error: ";
  if (error == nullptr) {
    return std::string(fallback);
  }
  std::string_view message = error;
  message.remove_prefix(message.starts_with(kInvalidInput) ? kInvalidInput.size() : 0);
  return std::string(message);
}

void Query(duckdb_connection connection, const std::string& sql, Result& result) {
  if (duckdb_query(connection, sql.c_str(), &result.result) == DuckDBError) {
    throw BackendError(ErrorMessage(duckdb_result_error(&result.result), "DuckDB query failed"));
  }
}

void Query(duckdb_connection connection, const std::string& sql) {
  Result result;
  Query(connection, sql, result);
}

// BigQuery evaluates timestamps in UTC; setup must run on the same connection.
void RunSetup(duckdb_connection connection, const std::vector<std::string>& setup) {
  Query(connection, "SET TimeZone = 'UTC'");
  for (const std::string& statement : setup) {
    Query(connection, statement);
  }
}

}  // namespace

json QueryResult::SchemaToJson() const {
  std::vector<FieldSchema> given = schema;
  SetModesGiven(given);
  return bigquery_emulator_duckdb::SchemaToJson(given);
}

bool HasTimestampField(const std::vector<FieldSchema>& schema) {
  return std::ranges::any_of(schema, [](const FieldSchema& field) {
    return field.type == FieldType::kTimestamp ||
           field.range_element_type == FieldType::kTimestamp || HasTimestampField(field.fields);
  });
}

json TimestampsAsSeconds(const std::vector<FieldSchema>& schema, const json& row) {
  return json{{"f", CellsAsSeconds(schema, row.at("f"))}};
}

struct Backend::Database {
  Handle<duckdb_database, duckdb_close> handle;
  Database(const std::optional<std::string>& session_user,
           const backend_functions::JavaScriptLimits& javascript) {
    if (session_user && session_user->empty()) {
      throw std::invalid_argument("session_user must not be empty");
    }
    char* error = nullptr;
    const duckdb_state state = duckdb_open_ext(nullptr, handle.out(), nullptr, &error);
    DuckString const message(error);
    if (state == DuckDBError) {
      throw BackendError(message ? message.get() : "DuckDB failed to open database");
    }
    RegisterBackendFunctions(handle.get(), session_user, javascript);
    RegisterPostgresScanner(handle.get());
  }

  [[nodiscard]] Connection Connect() const {
    Connection connection;
    if (duckdb_connect(handle.get(), connection.out()) == DuckDBError) {
      throw BackendError("DuckDB failed to connect");
    }
    return Connection(connection.release());
  }
};

struct Backend::Session {
  Connection connection;
  bool in_transaction = false;
  explicit Session(Database& database) : connection(database.Connect()) {}
};

Backend::Backend(const std::optional<std::string>& session_user,
                 const backend_functions::JavaScriptLimits& javascript)
    : db_(std::make_shared<Database>(session_user, javascript)) {}

Backend::Backend(std::shared_ptr<Database> database)
    : db_(std::move(database)), session_(std::make_unique<Session>(*db_)) {}

std::unique_ptr<Backend> Backend::NewSession() {
  return std::unique_ptr<Backend>(new Backend(db_));
}

Backend::~Backend() = default;

void Backend::Transaction(const std::string& statement) {
  if (!session_) {
    throw BackendError("Transactions require a session");
  }
  Query(session_->connection.get(), statement);
  session_->in_transaction = statement == "BEGIN TRANSACTION";
}

QueryResult Backend::Execute(const std::string& sql, const std::vector<std::string>& setup,
                             bool null_arrays) {
  return ExecuteAll({sql}, setup, null_arrays);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): setup runs before the statements.
QueryResult Backend::ExecuteAll(const std::vector<std::string>& statements,
                                const std::vector<std::string>& setup, bool null_arrays) {
  if (statements.empty()) {
    throw BackendError("No statement to execute");
  }
  Connection const owned = session_ ? Connection{} : db_->Connect();
  auto* const connection = session_ ? session_->connection.get() : owned.get();
  RunSetup(connection, setup);
  // A transaction left open by a failed statement is rolled back when the connection closes.
  for (size_t i = 0; i + 1 < statements.size(); ++i) {
    Query(connection, statements.at(i));
  }
  Result result;
  Query(connection, statements.back(), result);
  const auto statement_type = duckdb_result_statement_type(result.result);
  QueryResult query_result;
  if (!ProducesResultSet(statement_type)) {
    if (IsDml(statement_type)) {
      query_result.affected_rows = static_cast<int64_t>(duckdb_rows_changed(&result.result));
    }
    return query_result;
  }
  query_result.has_rows = true;
  for (idx_t i = 0; i < duckdb_column_count(&result.result); ++i) {
    LogicalType const type(duckdb_column_logical_type(&result.result, i));
    query_result.schema.push_back(ToFieldSchema(duckdb_column_name(&result.result, i), type.get()));
  }
  while (true) {
    Chunk const chunk(duckdb_fetch_chunk(result.result));
    if (chunk.get() == nullptr) {
      break;
    }
    for (idx_t row = 0; row < duckdb_data_chunk_get_size(chunk.get()); ++row) {
      json cells = json::array();
      for (idx_t column = 0; column < duckdb_data_chunk_get_column_count(chunk.get()); ++column) {
        duckdb_vector vector = duckdb_data_chunk_get_vector(chunk.get(), column);
        LogicalType const type(duckdb_vector_get_column_type(vector));
        cells.push_back(ToCell(vector, type.get(), row, null_arrays));
      }
      query_result.rows.push_back(json{{"f", std::move(cells)}});
    }
  }
  return query_result;
}

void Backend::ExecuteDdl(const std::string& sql,
                         const std::vector<std::string>& metadata_statements,
                         const std::string& skip_query, const std::vector<std::string>& setup) {
  Connection const owned = session_ ? Connection{} : db_->Connect();
  auto* const connection = session_ ? session_->connection.get() : owned.get();
  RunSetup(connection, setup);
  const bool own_transaction = !session_ || !session_->in_transaction;
  if (own_transaction) {
    Query(connection, "BEGIN TRANSACTION");
  }
  try {
    if (!skip_query.empty()) {
      Result existing;
      Query(connection, skip_query, existing);
      if (duckdb_row_count(&existing.result) != 0) {
        if (own_transaction) {
          Query(connection, "COMMIT");
        }
        return;
      }
    }
    Query(connection, sql);
    for (const std::string& statement : metadata_statements) {
      Query(connection, statement);
    }
    if (own_transaction) {
      Query(connection, "COMMIT");
    }
  } catch (...) {
    if (own_transaction && session_) {
      Query(connection, "ROLLBACK");
    }
    throw;
  }
}

QueryResult Backend::Prepare(const std::string& sql, const std::vector<std::string>& setup) {
  Connection const owned = session_ ? Connection{} : db_->Connect();
  auto* const connection = session_ ? session_->connection.get() : owned.get();
  RunSetup(connection, setup);
  // Preparing binds names and types without running the statement, which is what a dry run
  // needs: the query is validated and its result schema is known, but nothing is read or
  // written.
  Prepared prepared;
  if (duckdb_prepare(connection, sql.c_str(), prepared.out()) == DuckDBError) {
    throw BackendError(
        ErrorMessage(duckdb_prepare_error(prepared.get()), "DuckDB failed to prepare the query"));
  }

  QueryResult query_result;
  if (!ProducesResultSet(duckdb_prepared_statement_type(prepared.get()))) {
    return query_result;
  }
  query_result.has_rows = true;
  for (idx_t i = 0; i < duckdb_prepared_statement_column_count(prepared.get()); ++i) {
    DuckString const name(duckdb_prepared_statement_column_name(prepared.get(), i));
    LogicalType const type(duckdb_prepared_statement_column_logical_type(prepared.get(), i));
    query_result.schema.push_back(ToFieldSchema(name.get(), type.get()));
  }
  return query_result;
}

std::optional<std::string> Backend::UnreadableColumnError(const std::string& relation) {
  Connection const owned = session_ ? Connection{} : db_->Connect();
  auto* const connection = session_ ? session_->connection.get() : owned.get();
  Prepared prepared;
  const std::string sql = "SELECT * FROM " + relation;
  if (duckdb_prepare(connection, sql.c_str(), prepared.out()) == DuckDBError) {
    throw BackendError(
        ErrorMessage(duckdb_prepare_error(prepared.get()), "DuckDB failed to prepare the query"));
  }
  for (idx_t i = 0; i < duckdb_prepared_statement_column_count(prepared.get()); ++i) {
    LogicalType const type(duckdb_prepared_statement_column_logical_type(prepared.get(), i));
    if (const std::string name = UnsupportedType(type.get()); !name.empty()) {
      DuckString const column(duckdb_prepared_statement_column_name(prepared.get(), i));
      return "Column " + std::string(column.get()) + " has the DuckDB type " + name +
             ", which the emulator does not support";
    }
  }
  return std::nullopt;
}

std::optional<std::string> Backend::NumericReadQuery(const std::string& relation) {
  Connection const owned = session_ ? Connection{} : db_->Connect();
  auto* const connection = session_ ? session_->connection.get() : owned.get();
  Prepared prepared;
  const std::string sql = "SELECT * FROM " + relation;
  if (duckdb_prepare(connection, sql.c_str(), prepared.out()) == DuckDBError) {
    throw BackendError(
        ErrorMessage(duckdb_prepare_error(prepared.get()), "DuckDB failed to prepare the query"));
  }
  std::vector<std::string> columns;
  bool changed = false;
  for (idx_t i = 0; i < duckdb_prepared_statement_column_count(prepared.get()); ++i) {
    DuckString const name(duckdb_prepared_statement_column_name(prepared.get(), i));
    LogicalType const type(duckdb_prepared_statement_column_logical_type(prepared.get(), i));
    const std::string column = QuoteIdentifier(name.get());
    const auto converted = ReadNumericStorage(column, type.get(), 0);
    changed |= converted.has_value();
    columns.push_back(converted.value_or(column) + " AS " + column);
  }
  return changed ? std::make_optional("SELECT " + Join(columns, ", ") + " FROM " + relation)
                 : std::nullopt;
}

std::vector<std::pair<size_t, std::string>> Backend::InsertRows(
    const std::vector<std::string>& statements, bool skip_invalid_rows) {
  Connection const owned = session_ ? Connection{} : db_->Connect();
  auto* const connection = session_ ? session_->connection.get() : owned.get();
  RunSetup(connection, {});
  std::vector<std::pair<size_t, std::string>> errors;
  if (!skip_invalid_rows) {
    Query(connection, "BEGIN TRANSACTION");
  }
  for (size_t i = 0; i < statements.size(); ++i) {
    Result result;
    if (duckdb_query(connection, statements.at(i).c_str(), &result.result) == DuckDBError) {
      const char* error = duckdb_result_error(&result.result);
      errors.emplace_back(i, error != nullptr ? error : "DuckDB insert failed");
      if (!skip_invalid_rows) {
        Query(connection, "ROLLBACK");
        return errors;
      }
    }
  }
  if (!skip_invalid_rows) {
    Query(connection, "COMMIT");
  }
  return errors;
}

}  // namespace bigquery_emulator_duckdb
