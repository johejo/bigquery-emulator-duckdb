#include "src/vector_value.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "duckdb.h"
#include "src/backend_error.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb {

bool IsNull(duckdb_vector vector, idx_t row) {
  uint64_t* validity = duckdb_vector_get_validity(vector);
  return validity != nullptr && !duckdb_validity_row_is_valid(validity, row);
}

ListElements::ListElements(duckdb_vector vector, duckdb_logical_type type, idx_t row)
    : type_(ElementType(type)) {
  if (duckdb_get_type_id(type) == DUCKDB_TYPE_ARRAY) {
    const idx_t length = duckdb_array_type_array_size(type);
    child_ = duckdb_array_vector_get_child(vector);
    entry_ = {.offset = row * length, .length = length};
  } else {
    child_ = duckdb_list_vector_get_child(vector);
    entry_ = VectorElement<duckdb_list_entry>(vector, row);
  }
}

bool IsFiniteBound(duckdb_vector bound, duckdb_logical_type type, idx_t row) {
  return !IsNull(bound, row) &&
         (duckdb_get_type_id(type) == DUCKDB_TYPE_DATE
              ? duckdb_is_finite_date(VectorElement<duckdb_date>(bound, row))
              : duckdb_is_finite_timestamp(VectorElement<duckdb_timestamp>(bound, row)));
}

Value VectorValue(duckdb_vector vector, duckdb_logical_type type, idx_t row) {
  if (IsNull(vector, row)) {
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
      const ListElements elements(vector, type, row);
      std::vector<Value> values;
      std::vector<duckdb_value> handles;
      for (idx_t i = 0; i < elements.size(); ++i) {
        values.push_back(VectorValue(elements.child(), elements.type(), elements.row(i)));
        handles.push_back(values.back().get());
      }
      return Value(duckdb_get_type_id(type) == DUCKDB_TYPE_ARRAY
                       ? duckdb_create_array_value(elements.type(), handles.data(), handles.size())
                       : duckdb_create_list_value(elements.type(), handles.data(), handles.size()));
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

std::string ValueString(duckdb_value value) {
  DuckString const text(duckdb_get_varchar(value));
  if (!text) {
    throw BackendError("DuckDB failed to format a value");
  }
  return text.get();
}

}  // namespace bigquery_emulator_duckdb
