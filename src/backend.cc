#include "src/backend.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "duckdb.h"
#include "googlesql/public/functions/distance.h"
#include "googlesql/public/functions/hash.h"
#include "googlesql/public/functions/json.h"
#include "googlesql/public/functions/json_internal.h"
#include "googlesql/public/functions/regexp.h"
#include "googlesql/public/functions/string.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/value.h"
#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// C API handles must be released even when materialization or a query throws.
template <typename T, void (*Destroy)(T*)>
class Handle {
 public:
  explicit Handle(T handle = nullptr) : handle_(handle) {}
  ~Handle() { Destroy(&handle_); }
  Handle(Handle&& other) noexcept : handle_(other.release()) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  T get() const { return handle_; }
  T* out() { return &handle_; }
  T release() { return std::exchange(handle_, nullptr); }

 private:
  T handle_;
};

using LogicalType = Handle<duckdb_logical_type, duckdb_destroy_logical_type>;
using Value = Handle<duckdb_value, duckdb_destroy_value>;
using Connection = Handle<duckdb_connection, duckdb_disconnect>;
using Prepared = Handle<duckdb_prepared_statement, duckdb_destroy_prepare>;
using Chunk = Handle<duckdb_data_chunk, duckdb_destroy_data_chunk>;
struct DuckFree {
  void operator()(const void* pointer) const { duckdb_free(const_cast<void*>(pointer)); }
};
using DuckString = std::unique_ptr<const char, DuckFree>;

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

// Maps a DuckDB type to the BigQuery type name used in TableFieldSchema.type.
std::string ToBigQueryTypeName(duckdb_logical_type type) {
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BOOLEAN:
      return "BOOLEAN";
    case DUCKDB_TYPE_TINYINT:
    case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_INTEGER:
    case DUCKDB_TYPE_BIGINT:
    case DUCKDB_TYPE_UTINYINT:
    case DUCKDB_TYPE_USMALLINT:
    case DUCKDB_TYPE_UINTEGER:
    case DUCKDB_TYPE_UBIGINT:
      return "INTEGER";
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UHUGEINT:
      return "BIGNUMERIC";
    case DUCKDB_TYPE_FLOAT:
    case DUCKDB_TYPE_DOUBLE:
      return "FLOAT";
    case DUCKDB_TYPE_DECIMAL:
      return duckdb_decimal_scale(type) <= 9 ? "NUMERIC" : "BIGNUMERIC";
    case DUCKDB_TYPE_VARCHAR:
    case DUCKDB_TYPE_UUID:
      return "STRING";
    case DUCKDB_TYPE_BLOB:
      return "BYTES";
    case DUCKDB_TYPE_DATE:
      return "DATE";
    case DUCKDB_TYPE_TIME:
    case DUCKDB_TYPE_TIME_TZ:
      return "TIME";
    // BigQuery TIMESTAMP is an absolute instant, which is DuckDB's TIMESTAMP WITH TIME ZONE.
    // BigQuery DATETIME is a civil time, which is DuckDB's plain TIMESTAMP.
    case DUCKDB_TYPE_TIMESTAMP_TZ:
      return "TIMESTAMP";
    case DUCKDB_TYPE_TIMESTAMP:
    case DUCKDB_TYPE_TIMESTAMP_S:
    case DUCKDB_TYPE_TIMESTAMP_MS:
    case DUCKDB_TYPE_TIMESTAMP_NS:
      return "DATETIME";
    case DUCKDB_TYPE_INTERVAL:
      return "INTERVAL";
    case DUCKDB_TYPE_STRUCT:
      return "RECORD";
    case DUCKDB_TYPE_LIST:
    case DUCKDB_TYPE_ARRAY:
      return ToBigQueryTypeName(ElementType(type).get());
    default:
      return "STRING";
  }
}

bool IsListLike(duckdb_logical_type type) {
  return duckdb_get_type_id(type) == DUCKDB_TYPE_LIST ||
         duckdb_get_type_id(type) == DUCKDB_TYPE_ARRAY;
}

FieldSchema ToFieldSchema(const std::string& name, duckdb_logical_type type) {
  FieldSchema field;
  field.name = name;
  field.type = ToBigQueryTypeName(type);
  field.mode = IsListLike(type) ? "REPEATED" : "NULLABLE";
  LogicalType element(IsListLike(type) ? ElementType(type).release() : nullptr);
  duckdb_logical_type scalar_type = element.get() ? element.get() : type;
  if (duckdb_get_type_id(scalar_type) == DUCKDB_TYPE_STRUCT) {
    for (idx_t i = 0; i < duckdb_struct_type_child_count(scalar_type); ++i) {
      DuckString child_name(duckdb_struct_type_child_name(scalar_type, i));
      LogicalType child_type(duckdb_struct_type_child_type(scalar_type, i));
      field.fields.push_back(ToFieldSchema(child_name.get(), child_type.get()));
    }
  }
  return field;
}

// BigQuery's default TIMESTAMP encoding: a decimal string of seconds since the Unix epoch.
std::string EpochSecondsString(int64_t micros) {
  const bool negative = micros < 0;
  const uint64_t magnitude =
      negative ? static_cast<uint64_t>(-(micros + 1)) + 1 : static_cast<uint64_t>(micros);
  const uint64_t seconds = magnitude / 1000000;
  const uint64_t fraction = magnitude % 1000000;
  std::string result = negative ? "-" : "";
  result += std::to_string(seconds);
  if (fraction != 0) {
    std::string digits = std::to_string(fraction);
    digits.insert(0, 6 - digits.size(), '0');
    while (digits.back() == '0') {
      digits.pop_back();
    }
    result += "." + digits;
  }
  return result;
}

template <typename T>
T VectorElement(duckdb_vector vector, idx_t row) {
  return static_cast<const T*>(duckdb_vector_get_data(vector))[row];
}

std::string VectorString(duckdb_vector vector, idx_t row) {
  duckdb_string_t value = VectorElement<duckdb_string_t>(vector, row);
  return {duckdb_string_t_data(&value), duckdb_string_t_length(value)};
}

std::string ValueString(duckdb_value value) {
  DuckString text(duckdb_get_varchar(value));
  if (!text) {
    throw BackendError("DuckDB failed to format a value");
  }
  return text.get();
}

std::string Base64(const std::string& bytes) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const auto a = static_cast<unsigned char>(bytes[i]);
    const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
    const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
    result += alphabet[a >> 2];
    result += alphabet[((a & 3) << 4) | (b >> 4)];
    result += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
    result += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
  }
  return result;
}

// Reconstruct values through the C API so DuckDB formats scalar values itself.
Value VectorValue(duckdb_vector vector, duckdb_logical_type type, idx_t row) {
  uint64_t* validity = duckdb_vector_get_validity(vector);
  if (validity && !duckdb_validity_row_is_valid(validity, row)) {
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
          number = {static_cast<uint64_t>(value), value < 0 ? -1 : 0};
          break;
        }
        case DUCKDB_TYPE_INTEGER: {
          const int64_t value = VectorElement<int32_t>(vector, row);
          number = {static_cast<uint64_t>(value), value < 0 ? -1 : 0};
          break;
        }
        case DUCKDB_TYPE_BIGINT: {
          const int64_t value = VectorElement<int64_t>(vector, row);
          number = {static_cast<uint64_t>(value), value < 0 ? -1 : 0};
          break;
        }
        default:
          number = VectorElement<duckdb_hugeint>(vector, row);
      }
      return Value(
          duckdb_create_decimal({duckdb_decimal_width(type), duckdb_decimal_scale(type), number}));
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
      return Value(duckdb_create_uuid(
          {uuid.lower, static_cast<uint64_t>(uuid.upper) ^ (uint64_t{1} << 63)}));
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
      const duckdb_list_entry entry = array ? duckdb_list_entry{row * size, size}
                                            : VectorElement<duckdb_list_entry>(vector, row);
      duckdb_vector child =
          array ? duckdb_array_vector_get_child(vector) : duckdb_list_vector_get_child(vector);
      LogicalType child_type = ElementType(type);
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
        LogicalType child_type(duckdb_struct_type_child_type(type, i));
        values.push_back(
            VectorValue(duckdb_struct_vector_get_child(vector, i), child_type.get(), row));
        handles.push_back(values.back().get());
      }
      return Value(duckdb_create_struct_value(type, handles.data()));
    }
    case DUCKDB_TYPE_MAP: {
      const auto entry = VectorElement<duckdb_list_entry>(vector, row);
      duckdb_vector entries = duckdb_list_vector_get_child(vector);
      LogicalType key_type(duckdb_map_type_key_type(type));
      LogicalType value_type(duckdb_map_type_value_type(type));
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
      LogicalType member_type(duckdb_union_type_member_type(type, tag));
      Value member =
          VectorValue(duckdb_struct_vector_get_child(vector, tag + 1), member_type.get(), row);
      return Value(duckdb_create_union_value(type, tag, member.get()));
    }
    case DUCKDB_TYPE_BIT: {
      std::string bytes = VectorString(vector, row);
      return Value(duckdb_create_bit({reinterpret_cast<uint8_t*>(bytes.data()), bytes.size()}));
    }
    default:
      throw BackendError("Unsupported DuckDB result type: " +
                         std::to_string(duckdb_get_type_id(type)));
  }
}

json ToCell(duckdb_vector vector, duckdb_logical_type type, idx_t row) {
  uint64_t* validity = duckdb_vector_get_validity(vector);
  const bool is_null = validity && !duckdb_validity_row_is_valid(validity, row);
  if (IsListLike(type)) {
    json elements = json::array();
    if (!is_null) {
      LogicalType child_type = ElementType(type);
      const bool array = duckdb_get_type_id(type) == DUCKDB_TYPE_ARRAY;
      const idx_t size = array ? duckdb_array_type_array_size(type) : 0;
      const duckdb_list_entry entry = array ? duckdb_list_entry{row * size, size}
                                            : VectorElement<duckdb_list_entry>(vector, row);
      duckdb_vector child =
          array ? duckdb_array_vector_get_child(vector) : duckdb_list_vector_get_child(vector);
      for (idx_t i = 0; i < entry.length; ++i) {
        elements.push_back(ToCell(child, child_type.get(), entry.offset + i));
      }
    }
    return json{{"v", std::move(elements)}};
  }
  if (is_null) {
    return json{{"v", nullptr}};
  }
  json value;
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_STRUCT: {
      json fields = json::array();
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        LogicalType child_type(duckdb_struct_type_child_type(type, i));
        fields.push_back(ToCell(duckdb_struct_vector_get_child(vector, i), child_type.get(), row));
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
        Value timestamp(duckdb_create_timestamp({infinite ? nanos : nanos / 1000}));
        text = ValueString(timestamp.get());
      } else {
        text = ValueString(VectorValue(vector, type, row).get());
      }
      if (text.size() > 10 && text[10] == ' ') {
        text[10] = 'T';
      }
      value = std::move(text);
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
  if (field.type == "TIMESTAMP") {
    return EpochSecondsString(std::stoll(value.get<std::string>()));
  }
  if (field.type == "RECORD") {
    return json{{"f", CellsAsSeconds(field.fields, value.at("f"))}};
  }
  return value;
}

json CellsAsSeconds(const std::vector<FieldSchema>& schema, const json& cells) {
  json result = json::array();
  for (size_t i = 0; i < schema.size() && i < cells.size(); ++i) {
    const FieldSchema& field = schema[i];
    const json& value = cells[i].at("v");
    if (field.mode != "REPEATED" || value.is_null()) {
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

void Query(duckdb_connection connection, const std::string& sql, Result& result) {
  if (duckdb_query(connection, sql.c_str(), &result.result) == DuckDBError) {
    const char* error = duckdb_result_error(&result.result);
    throw BackendError(error ? error : "DuckDB query failed");
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

  [[nodiscard]] bool Bool(idx_t column) const {
    return static_cast<bool*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(input_, column)))[row_];
  }

  [[nodiscard]] bool IsNull(idx_t column) const {
    uint64_t* validity = duckdb_vector_get_validity(duckdb_data_chunk_get_vector(input_, column));
    return validity != nullptr && !duckdb_validity_row_is_valid(validity, row_);
  }

 private:
  duckdb_data_chunk input_;
  idx_t row_;
};

void SetResult(duckdb_vector output, idx_t row, int64_t value) {
  static_cast<int64_t*>(duckdb_vector_get_data(output))[row] = value;
}

void SetResult(duckdb_vector output, idx_t row, bool value) {
  static_cast<bool*>(duckdb_vector_get_data(output))[row] = value;
}

void SetResult(duckdb_vector output, idx_t row, double value) {
  static_cast<double*>(duckdb_vector_get_data(output))[row] = value;
}

void SetResult(duckdb_vector output, idx_t row, const std::string& value) {
  duckdb_vector_assign_string_element_len(output, row, value.data(), value.size());
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

void FarmFingerprint(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<int64_t> {
    return googlesql::functions::FarmFingerprint(arguments.String(0));
  });
}

void Sha512(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto hasher = googlesql::functions::Hasher::Create(googlesql::functions::Hasher::kSha512);
  EachRow(info, input, output,
          [&hasher](const Arguments& arguments) -> absl::StatusOr<std::string> {
            return hasher->Hash(arguments.String(0));
          });
}

void InitCap(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [input](const Arguments& arguments) {
    std::string out;
    absl::Status error;
    const bool ok =
        duckdb_data_chunk_get_column_count(input) == 1
            ? googlesql::functions::InitialCapitalizeDefault(arguments.String(0), &out, &error)
            : googlesql::functions::InitialCapitalize(arguments.String(0), arguments.String(1),
                                                      &out, &error);
    return ToStatusOr(ok, out, error);
  });
}

template <bool kBytes>
void EditDistance(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    return (kBytes ? googlesql::functions::EditDistanceBytes : googlesql::functions::EditDistance)(
        arguments.String(0), arguments.String(1), arguments.Int(2));
  });
}

// REGEXP_INSTR(source, regexp, position, occurrence, occurrence_position).
template <bool kBytes>
void RegexpInstr(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<int64_t> {
    const std::string pattern = arguments.String(1);
    auto regexp = kBytes ? googlesql::functions::MakeRegExpBytes(pattern)
                         : googlesql::functions::MakeRegExpUtf8(pattern);
    if (!regexp.ok()) {
      return regexp.status();
    }
    const int64_t occurrence_position = arguments.Int(4);
    if (occurrence_position != 0 && occurrence_position != 1) {
      return absl::OutOfRangeError(
          "Invalid return_position_after_match; it must be 0 or 1 (in REGEXP_INSTR)");
    }
    const std::string source = arguments.String(0);
    int64_t out = 0;
    absl::Status error;
    const bool ok = (*regexp)->Instr(
        {.input_str = source,
         .position_unit = kBytes ? googlesql::functions::RegExp::kBytes
                                 : googlesql::functions::RegExp::kUtf8Chars,
         .position = arguments.Int(2),
         .occurrence_index = arguments.Int(3),
         .return_position = occurrence_position == 0 ? googlesql::functions::RegExp::kStartOfMatch
                                                     : googlesql::functions::RegExp::kEndOfMatch,
         .out = &out},
        /*use_legacy_position_behavior=*/false, &error);
    return ToStatusOr(ok, out, error);
  });
}

// JSON goes in and out of the JSON functions as its text.
absl::StatusOr<googlesql::JSONValue> ParseJson(const std::string& text) {
  return googlesql::JSONValue::ParseJSONString(text);
}

// A JSON path argument, which is checked even when the JSON is NULL; null for a NULL path.
absl::StatusOr<std::unique_ptr<googlesql::functions::json_internal::StrictJSONPathIterator>>
JsonPathArgument(const Arguments& arguments, idx_t column, const std::string& function) {
  if (arguments.IsNull(column)) {
    return nullptr;
  }
  auto path =
      googlesql::functions::json_internal::StrictJSONPathIterator::Create(arguments.String(column));
  if (!path.ok()) {
    return absl::OutOfRangeError("Invalid input to " + function + ": " +
                                 std::string(path.status().message()));
  }
  return path;
}

// LAX_BOOL, LAX_INT64, LAX_FLOAT64 and LAX_STRING, which are NULL for JSON of another type.
template <typename T, absl::StatusOr<std::optional<T>> (*kConvert)(googlesql::JSONValueConstRef)>
void LaxConvert(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::optional<T>> {
    const auto document = ParseJson(arguments.String(0));
    if (!document.ok()) {
      return document.status();
    }
    return kConvert(document->GetConstRef());
  });
}

// JSON_KEYS(json, max_depth, mode), as the JSON array of the keys.
void JsonKeys(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  using googlesql::functions::json_internal::JsonPathOptions;
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const int64_t max_depth =
            arguments.IsNull(1) ? std::numeric_limits<int64_t>::max() : arguments.Int(1);
        if (max_depth <= 0) {
          return absl::OutOfRangeError("max_depth must be positive.");
        }
        if (arguments.IsNull(0) || arguments.IsNull(2)) {
          return std::nullopt;
        }
        const std::string mode = arguments.String(2);
        JsonPathOptions options = JsonPathOptions::kStrict;
        if (absl::EqualsIgnoreCase(mode, "lax")) {
          options = JsonPathOptions::kLax;
        } else if (absl::EqualsIgnoreCase(mode, "lax recursive")) {
          options = JsonPathOptions::kLaxRecursive;
        } else if (!absl::EqualsIgnoreCase(mode, "strict")) {
          return absl::OutOfRangeError("Invalid JSON mode specified");
        }
        const auto document = ParseJson(arguments.String(0));
        if (!document.ok()) {
          return document.status();
        }
        const auto keys = googlesql::functions::JsonKeys(
            document->GetConstRef(), {.path_options = options, .max_depth = max_depth});
        if (!keys.ok()) {
          return keys.status();
        }
        return json(*keys).dump();
      },
      /*nulls=*/false);
}

// JSON_REMOVE(json, path) for one path; a NULL path removes nothing.
void JsonRemove(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const auto path = JsonPathArgument(arguments, 1, "JSON_REMOVE");
        if (!path.ok()) {
          return path.status();
        }
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        auto document = ParseJson(arguments.String(0));
        if (!document.ok()) {
          return document.status();
        }
        if (*path != nullptr) {
          const auto removed = googlesql::functions::JsonRemove(document->GetRef(), **path);
          if (!removed.ok()) {
            return removed.status();
          }
        }
        return document->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// JSON_SET(json, path, value, create_if_missing) for one path, with the value as JSON text and
// NULL as JSON null; a NULL path or create_if_missing sets nothing.
void JsonSet(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const auto path = JsonPathArgument(arguments, 1, "JSON_SET");
        if (!path.ok()) {
          return path.status();
        }
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        auto document = ParseJson(arguments.String(0));
        if (!document.ok()) {
          return document.status();
        }
        if (*path == nullptr || arguments.IsNull(3)) {
          return document->GetConstRef().ToString();
        }
        auto value = arguments.IsNull(2) ? googlesql::JSONValue() : ParseJson(arguments.String(2));
        if (!value.ok()) {
          return value.status();
        }
        const absl::Status status = googlesql::functions::JsonSet(
            document->GetRef(), **path, googlesql::Value::Json(*std::move(value)),
            arguments.Bool(3), googlesql::LanguageOptions(), /*canonicalize_zero=*/true);
        if (!status.ok()) {
          return absl::OutOfRangeError("Invalid input to JSON_SET: " +
                                       std::string(status.message()));
        }
        return document->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// JSON_STRIP_NULLS(json, path, include_arrays, remove_empty); a NULL argument past the JSON
// strips nothing.
void JsonStripNulls(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const auto path = JsonPathArgument(arguments, 1, "JSON_STRIP_NULLS");
        if (!path.ok()) {
          return path.status();
        }
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        auto document = ParseJson(arguments.String(0));
        if (!document.ok()) {
          return document.status();
        }
        if (*path != nullptr && !arguments.IsNull(2) && !arguments.IsNull(3)) {
          absl::Status status = googlesql::functions::JsonStripNulls(
              document->GetRef(), **path, arguments.Bool(2), arguments.Bool(3));
          if (!status.ok()) {
            return status;
          }
        }
        return document->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// JSON_OBJECT(keys, values), with the keys and the values as JSON arrays.
void JsonObject(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::string> {
        const auto invalid = [](absl::string_view message) {
          return absl::OutOfRangeError(std::string("Invalid input to JSON_OBJECT: ") +
                                       std::string(message));
        };
        if (arguments.IsNull(0)) {
          return invalid("The keys array cannot be NULL");
        }
        if (arguments.IsNull(1)) {
          return invalid("The values array cannot be NULL");
        }
        const auto keys = ParseJson(arguments.String(0));
        if (!keys.ok()) {
          return keys.status();
        }
        const auto values = ParseJson(arguments.String(1));
        if (!values.ok()) {
          return values.status();
        }
        if (keys->GetConstRef().GetArraySize() != values->GetConstRef().GetArraySize()) {
          return invalid("The number of keys and values must match");
        }
        std::vector<std::string> key_strings;
        for (const googlesql::JSONValueConstRef key : keys->GetConstRef().GetArrayElements()) {
          if (!key.IsString()) {
            return invalid("A key cannot be NULL");
          }
          key_strings.push_back(key.GetString());
        }
        std::vector<googlesql::Value> value_list;
        for (const googlesql::JSONValueConstRef value : values->GetConstRef().GetArrayElements()) {
          value_list.push_back(googlesql::Value::Json(googlesql::JSONValue::CopyFrom(value)));
        }
        const std::vector<absl::string_view> key_views(key_strings.begin(), key_strings.end());
        std::vector<const googlesql::Value*> value_pointers;
        value_pointers.reserve(value_list.size());
        for (const googlesql::Value& value : value_list) {
          value_pointers.push_back(&value);
        }
        googlesql::functions::JsonObjectBuilder builder(googlesql::LanguageOptions(),
                                                        /*canonicalize_zero=*/true);
        const auto object =
            googlesql::functions::JsonObject(key_views, absl::MakeSpan(value_pointers), builder);
        if (!object.ok()) {
          return invalid(object.status().message());
        }
        return object->GetConstRef().ToString();
      },
      /*nulls=*/false);
}

// Registers `function` under `name`, taking `parameters` and returning `result`. Unless `nulls`
// is false, DuckDB makes the result NULL for a NULL argument without calling `function`.
void Register(duckdb_connection connection, const char* name,
              std::initializer_list<duckdb_type> parameters, duckdb_type result,
              duckdb_scalar_function_t function, bool nulls = true) {
  Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), name);
  for (const duckdb_type parameter : parameters) {
    LogicalType type(duckdb_create_logical_type(parameter));
    duckdb_scalar_function_add_parameter(scalar.get(), type.get());
  }
  LogicalType type(duckdb_create_logical_type(result));
  duckdb_scalar_function_set_return_type(scalar.get(), type.get());
  duckdb_scalar_function_set_function(scalar.get(), function);
  if (!nulls) {
    duckdb_scalar_function_set_special_handling(scalar.get());
  }
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError(std::string("DuckDB failed to register ") + name);
  }
}

// The functions the translator calls beyond DuckDB's own, with GoogleSQL's implementations;
// see src/functions.cc.
void RegisterFunctions(duckdb_database database) {
  Connection connection;
  if (duckdb_connect(database, connection.out()) == DuckDBError) {
    throw BackendError("DuckDB failed to connect");
  }
  constexpr duckdb_type kBlob = DUCKDB_TYPE_BLOB;
  constexpr duckdb_type kVarchar = DUCKDB_TYPE_VARCHAR;
  constexpr duckdb_type kBigint = DUCKDB_TYPE_BIGINT;
  constexpr duckdb_type kBoolean = DUCKDB_TYPE_BOOLEAN;
  Register(connection.get(), "bq_farm_fingerprint", {kBlob}, kBigint, FarmFingerprint);
  Register(connection.get(), "bq_sha512", {kBlob}, kBlob, Sha512);
  Register(connection.get(), "bq_initcap", {kVarchar}, kVarchar, InitCap);
  Register(connection.get(), "bq_initcap_delimiters", {kVarchar, kVarchar}, kVarchar, InitCap);
  Register(connection.get(), "bq_edit_distance", {kVarchar, kVarchar, kBigint}, kBigint,
           EditDistance<false>);
  Register(connection.get(), "bq_edit_distance_bytes", {kBlob, kBlob, kBigint}, kBigint,
           EditDistance<true>);
  Register(connection.get(), "bq_regexp_instr", {kVarchar, kVarchar, kBigint, kBigint, kBigint},
           kBigint, RegexpInstr<false>);
  Register(connection.get(), "bq_regexp_instr_bytes", {kBlob, kBlob, kBigint, kBigint, kBigint},
           kBigint, RegexpInstr<true>);
  Register(connection.get(), "bq_lax_bool", {kVarchar}, kBoolean,
           LaxConvert<bool, googlesql::functions::LaxConvertJsonToBool>);
  Register(connection.get(), "bq_lax_int64", {kVarchar}, kBigint,
           LaxConvert<int64_t, googlesql::functions::LaxConvertJsonToInt64>);
  Register(connection.get(), "bq_lax_float64", {kVarchar}, DUCKDB_TYPE_DOUBLE,
           LaxConvert<double, googlesql::functions::LaxConvertJsonToFloat64>);
  Register(connection.get(), "bq_lax_string", {kVarchar}, kVarchar,
           LaxConvert<std::string, googlesql::functions::LaxConvertJsonToString>);
  Register(connection.get(), "bq_json_keys", {kVarchar, kBigint, kVarchar}, kVarchar, JsonKeys,
           /*nulls=*/false);
  Register(connection.get(), "bq_json_remove", {kVarchar, kVarchar}, kVarchar, JsonRemove,
           /*nulls=*/false);
  Register(connection.get(), "bq_json_set", {kVarchar, kVarchar, kVarchar, kBoolean}, kVarchar,
           JsonSet, /*nulls=*/false);
  Register(connection.get(), "bq_json_strip_nulls", {kVarchar, kVarchar, kBoolean, kBoolean},
           kVarchar, JsonStripNulls, /*nulls=*/false);
  Register(connection.get(), "bq_json_object", {kVarchar, kVarchar}, kVarchar, JsonObject,
           /*nulls=*/false);
}

}  // namespace

json QueryResult::SchemaToJson() const { return bigquery_emulator_duckdb::SchemaToJson(schema); }

bool HasTimestampField(const std::vector<FieldSchema>& schema) {
  for (const FieldSchema& field : schema) {
    if (field.type == "TIMESTAMP" || HasTimestampField(field.fields)) {
      return true;
    }
  }
  return false;
}

json TimestampsAsSeconds(const std::vector<FieldSchema>& schema, const json& row) {
  return json{{"f", CellsAsSeconds(schema, row.at("f"))}};
}

struct Backend::Database {
  Handle<duckdb_database, duckdb_close> handle;
  Database() {
    char* error = nullptr;
    const duckdb_state state = duckdb_open_ext(nullptr, handle.out(), nullptr, &error);
    DuckString message(error);
    if (state == DuckDBError) {
      throw BackendError(message ? message.get() : "DuckDB failed to open database");
    }
    RegisterFunctions(handle.get());
  }

  Connection Connect() const {
    Connection connection;
    if (duckdb_connect(handle.get(), connection.out()) == DuckDBError) {
      throw BackendError("DuckDB failed to connect");
    }
    return Connection(connection.release());
  }
};

Backend::Backend() : db_(std::make_unique<Database>()) {}

Backend::~Backend() = default;

QueryResult Backend::Execute(const std::string& sql, const std::vector<std::string>& setup) {
  return ExecuteAll({sql}, setup);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): setup runs before the statements.
QueryResult Backend::ExecuteAll(const std::vector<std::string>& statements,
                                const std::vector<std::string>& setup) {
  if (statements.empty()) {
    throw BackendError("No statement to execute");
  }
  Connection connection = db_->Connect();
  RunSetup(connection.get(), setup);
  // A transaction left open by a failed statement is rolled back when the connection closes.
  for (size_t i = 0; i + 1 < statements.size(); ++i) {
    Query(connection.get(), statements[i]);
  }
  Result result;
  Query(connection.get(), statements.back(), result);
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
    LogicalType type(duckdb_column_logical_type(&result.result, i));
    query_result.schema.push_back(ToFieldSchema(duckdb_column_name(&result.result, i), type.get()));
  }
  while (true) {
    Chunk chunk(duckdb_fetch_chunk(result.result));
    if (!chunk.get()) {
      break;
    }
    for (idx_t row = 0; row < duckdb_data_chunk_get_size(chunk.get()); ++row) {
      json cells = json::array();
      for (idx_t column = 0; column < duckdb_data_chunk_get_column_count(chunk.get()); ++column) {
        duckdb_vector vector = duckdb_data_chunk_get_vector(chunk.get(), column);
        LogicalType type(duckdb_vector_get_column_type(vector));
        cells.push_back(ToCell(vector, type.get(), row));
      }
      query_result.rows.push_back(json{{"f", std::move(cells)}});
    }
  }
  return query_result;
}

void Backend::CreateView(const std::string& sql,
                         const std::vector<std::string>& metadata_statements,
                         const std::string& existence_query,
                         const std::vector<std::string>& setup) {
  Connection connection = db_->Connect();
  RunSetup(connection.get(), setup);
  Query(connection.get(), "BEGIN TRANSACTION");
  if (!existence_query.empty()) {
    Result existing;
    Query(connection.get(), existence_query, existing);
    if (duckdb_row_count(&existing.result) != 0) {
      Query(connection.get(), "COMMIT");
      return;
    }
  }
  Query(connection.get(), sql);
  for (const std::string& statement : metadata_statements) {
    Query(connection.get(), statement);
  }
  Query(connection.get(), "COMMIT");
}

QueryResult Backend::Prepare(const std::string& sql, const std::vector<std::string>& setup) {
  Connection connection = db_->Connect();
  RunSetup(connection.get(), setup);
  // Preparing binds names and types without running the statement, which is what a dry run
  // needs: the query is validated and its result schema is known, but nothing is read or
  // written.
  Prepared prepared;
  if (duckdb_prepare(connection.get(), sql.c_str(), prepared.out()) == DuckDBError) {
    const char* error = duckdb_prepare_error(prepared.get());
    throw BackendError(error ? error : "DuckDB failed to prepare the query");
  }

  QueryResult query_result;
  if (!ProducesResultSet(duckdb_prepared_statement_type(prepared.get()))) {
    return query_result;
  }
  query_result.has_rows = true;
  for (idx_t i = 0; i < duckdb_prepared_statement_column_count(prepared.get()); ++i) {
    DuckString name(duckdb_prepared_statement_column_name(prepared.get(), i));
    LogicalType type(duckdb_prepared_statement_column_logical_type(prepared.get(), i));
    query_result.schema.push_back(ToFieldSchema(name.get(), type.get()));
  }
  return query_result;
}

std::vector<std::pair<size_t, std::string>> Backend::InsertRows(
    const std::vector<std::string>& statements, bool skip_invalid_rows) {
  Connection connection = db_->Connect();
  RunSetup(connection.get(), {});
  std::vector<std::pair<size_t, std::string>> errors;
  if (!skip_invalid_rows) {
    Query(connection.get(), "BEGIN TRANSACTION");
  }
  for (size_t i = 0; i < statements.size(); ++i) {
    Result result;
    if (duckdb_query(connection.get(), statements[i].c_str(), &result.result) == DuckDBError) {
      const char* error = duckdb_result_error(&result.result);
      errors.emplace_back(i, error ? error : "DuckDB insert failed");
      if (!skip_invalid_rows) {
        Query(connection.get(), "ROLLBACK");
        return errors;
      }
    }
  }
  if (!skip_invalid_rows) {
    Query(connection.get(), "COMMIT");
  }
  return errors;
}

std::string ExecuteScalarString(const std::string& sql) {
  Handle<duckdb_database, duckdb_close> database;
  if (duckdb_open(nullptr, database.out()) == DuckDBError) {
    throw BackendError("DuckDB failed to open database");
  }
  RegisterFunctions(database.get());
  Connection connection;
  if (duckdb_connect(database.get(), connection.out()) == DuckDBError) {
    throw BackendError("DuckDB failed to connect");
  }
  Result result;
  Query(connection.get(), sql, result);
  Chunk chunk(duckdb_fetch_chunk(result.result));
  if (!chunk.get() || duckdb_data_chunk_get_size(chunk.get()) == 0 ||
      duckdb_data_chunk_get_column_count(chunk.get()) == 0) {
    throw BackendError("DuckDB query returned no scalar value");
  }
  duckdb_vector vector = duckdb_data_chunk_get_vector(chunk.get(), 0);
  uint64_t* validity = duckdb_vector_get_validity(vector);
  if (validity && !duckdb_validity_row_is_valid(validity, 0)) {
    return "NULL";
  }
  LogicalType type(duckdb_vector_get_column_type(vector));
  if (duckdb_get_type_id(type.get()) == DUCKDB_TYPE_VARCHAR) {
    return VectorString(vector, 0);
  }
  return ValueString(VectorValue(vector, type.get(), 0).get());
}

}  // namespace bigquery_emulator_duckdb
