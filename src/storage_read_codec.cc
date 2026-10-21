#include "src/storage_read_codec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/time/time.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/numeric_value.h"
#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow.hpp"
#include "nanoarrow/nanoarrow_ipc.h"
#include "nanoarrow/nanoarrow_ipc.hpp"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

void Check(const absl::Status& status) {
  if (!status.ok()) {
    throw ApiError::Invalid(std::string(status.message()));
  }
}

void ArrowCheck(int code) {
  if (code != NANOARROW_OK) {
    throw ApiError::Internal("Arrow IPC operation failed: " + std::to_string(code));
  }
}

std::string Text(const json& value) {
  return value.is_string() ? value.get<std::string>() : value.dump();
}

std::string Bytes(const json& value) {
  std::string result;
  if (!absl::Base64Unescape(value.get<std::string>(), &result)) {
    throw ApiError::Internal("Invalid internal BYTES value");
  }
  return result;
}

// The bounds of a RANGE cell, each a cell of its element type or null where unbounded.
std::array<json, 2> RangeBounds(const json& value) {
  const auto bounds = RangeCellBounds(Text(value));
  if (!bounds) {
    throw ApiError::Internal("Invalid internal RANGE value");
  }
  std::array<json, 2> cells;
  std::ranges::transform(*bounds, cells.begin(),
                         [](const auto& bound) { return bound ? json(*bound) : json(nullptr); });
  return cells;
}

int64_t Temporal(const FieldSchema& field, const json& value) {
  const std::string text = Text(value);
  if (field.type == FieldType::kTimestamp) {
    return std::stoll(text);
  }
  if (field.type == FieldType::kDate) {
    int32_t days = 0;
    Check(googlesql::functions::ConvertStringToDate(text, &days));
    return days;
  }
  if (field.type == FieldType::kTime) {
    googlesql::TimeValue time;
    Check(googlesql::functions::ConvertStringToTime(text, googlesql::functions::kMicroseconds,
                                                    &time));
    return (((((time.Hour() * 60LL) + time.Minute()) * 60) + time.Second()) * 1000000) +
           time.Microseconds();
  }
  googlesql::DatetimeValue datetime;
  Check(googlesql::functions::ConvertStringToDatetime(text, googlesql::functions::kMicroseconds,
                                                      &datetime));
  absl::Time timestamp;
  Check(
      googlesql::functions::ConvertDatetimeToTimestamp(datetime, absl::UTCTimeZone(), &timestamp));
  return absl::ToUnixMicros(timestamp);
}

// GoogleSQL's packed decimals are signed little-endian scaled integers, as required by the
// Write API. Sign extend for Arrow's fixed width, or reverse for Avro's big-endian decimal.
std::string DecimalBytes(const FieldSchema& field, const json& value) {
  std::string result;
  if (field.type == FieldType::kNumeric) {
    const auto number = googlesql::NumericValue::FromStringStrict(Text(value));
    Check(number.status());
    result = number->SerializeAsProtoBytes();
  } else {
    const auto number = googlesql::BigNumericValue::FromStringStrict(Text(value));
    Check(number.status());
    result = number->SerializeAsProtoBytes();
  }
  const char pad = !result.empty() && (static_cast<unsigned char>(result.back()) & 0x80U) != 0
                       ? static_cast<char>(0xff)
                       : '\0';
  result.resize(field.type == FieldType::kNumeric ? 16 : 32, pad);
  return result;
}

void ArrowField(ArrowSchema* out, const FieldSchema& field) {
  if (field.precision || field.scale) {
    throw ApiError::Invalid("Storage Read does not support parameterized decimals");
  }
  ArrowSchemaInit(out);
  ArrowCheck(ArrowSchemaSetName(out, field.name.c_str()));
  if (field.mode == FieldMode::kRepeated) {
    ArrowCheck(ArrowSchemaSetType(out, NANOARROW_TYPE_LIST));
    FieldSchema item = field;
    item.name = "item";
    item.mode = FieldMode::kRequired;
    ArrowField(out->children[0], item);
    out->flags =
        static_cast<int64_t>(static_cast<uint64_t>(out->flags) & ~uint64_t{ARROW_FLAG_NULLABLE});
    return;
  }
  if (field.mode == FieldMode::kRequired) {
    out->flags =
        static_cast<int64_t>(static_cast<uint64_t>(out->flags) & ~uint64_t{ARROW_FLAG_NULLABLE});
  }
  switch (field.type) {
    case FieldType::kInteger:
      ArrowCheck(ArrowSchemaSetType(out, NANOARROW_TYPE_INT64));
      break;
    case FieldType::kFloat:
      ArrowCheck(ArrowSchemaSetType(out, NANOARROW_TYPE_DOUBLE));
      break;
    case FieldType::kBoolean:
      ArrowCheck(ArrowSchemaSetType(out, NANOARROW_TYPE_BOOL));
      break;
    case FieldType::kBytes:
      ArrowCheck(ArrowSchemaSetType(out, NANOARROW_TYPE_BINARY));
      break;
    case FieldType::kDate:
      ArrowCheck(ArrowSchemaSetType(out, NANOARROW_TYPE_DATE32));
      break;
    case FieldType::kTime:
      ArrowCheck(ArrowSchemaSetTypeDateTime(out, NANOARROW_TYPE_TIME64, NANOARROW_TIME_UNIT_MICRO,
                                            nullptr));
      break;
    case FieldType::kTimestamp:
    case FieldType::kDatetime:
      ArrowCheck(ArrowSchemaSetTypeDateTime(out, NANOARROW_TYPE_TIMESTAMP,
                                            NANOARROW_TIME_UNIT_MICRO,
                                            field.type == FieldType::kTimestamp ? "UTC" : nullptr));
      break;
    case FieldType::kNumeric:
    case FieldType::kBigNumeric:
      ArrowCheck(ArrowSchemaSetTypeDecimal(
          out,
          field.type == FieldType::kNumeric ? NANOARROW_TYPE_DECIMAL128 : NANOARROW_TYPE_DECIMAL256,
          field.type == FieldType::kNumeric ? 38 : 76, field.type == FieldType::kNumeric ? 9 : 38));
      break;
    case FieldType::kRecord:
      ArrowCheck(ArrowSchemaSetTypeStruct(out, static_cast<int64_t>(field.fields.size())));
      for (size_t i = 0; i < field.fields.size(); ++i) {
        ArrowField(out->children[i], field.fields.at(i));
      }
      break;
    case FieldType::kString:
      ArrowCheck(ArrowSchemaSetType(out, NANOARROW_TYPE_STRING));
      break;
    case FieldType::kRange:
      ArrowCheck(ArrowSchemaSetTypeStruct(out, 2));
      ArrowField(out->children[0], RangeBoundField(field, "start"));
      ArrowField(out->children[1], RangeBoundField(field, "end"));
      break;
    default:
      throw ApiError::Invalid("Storage Read does not support " +
                              std::string(FieldTypeName(field.type)));
  }
}

void ArrowCell(ArrowArray* array, const FieldSchema& field, const json& value) {
  if (field.mode == FieldMode::kRepeated) {
    FieldSchema item = field;
    item.mode = FieldMode::kRequired;
    for (const auto& cell : value) {
      ArrowCell(array->children[0], item, cell.at("v"));
    }
    ArrowCheck(ArrowArrayFinishElement(array));
    return;
  }
  if (value.is_null()) {
    ArrowCheck(ArrowArrayAppendNull(array, 1));
    return;
  }
  switch (field.type) {
    case FieldType::kRecord:
      for (size_t i = 0; i < field.fields.size(); ++i) {
        ArrowCell(array->children[i], field.fields.at(i), value.at("f").at(i).at("v"));
      }
      ArrowCheck(ArrowArrayFinishElement(array));
      break;
    case FieldType::kRange: {
      const auto bounds = RangeBounds(value);
      for (size_t i = 0; i < bounds.size(); ++i) {
        ArrowCell(array->children[i], RangeBoundField(field, ""), bounds.at(i));
      }
      ArrowCheck(ArrowArrayFinishElement(array));
      break;
    }
    case FieldType::kInteger:
      ArrowCheck(ArrowArrayAppendInt(array, std::stoll(Text(value))));
      break;
    case FieldType::kBoolean:
      ArrowCheck(ArrowArrayAppendInt(array, static_cast<int64_t>(Text(value) == "true")));
      break;
    case FieldType::kFloat:
      ArrowCheck(ArrowArrayAppendDouble(array, std::stod(Text(value))));
      break;
    case FieldType::kDate:
    case FieldType::kTime:
    case FieldType::kTimestamp:
    case FieldType::kDatetime:
      ArrowCheck(ArrowArrayAppendInt(array, Temporal(field, value)));
      break;
    case FieldType::kNumeric:
    case FieldType::kBigNumeric: {
      const std::string bytes = DecimalBytes(field, value);
      ArrowDecimal decimal{};
      ArrowDecimalInit(&decimal, field.type == FieldType::kNumeric ? 128 : 256,
                       field.type == FieldType::kNumeric ? 38 : 76,
                       field.type == FieldType::kNumeric ? 9 : 38);
      ArrowDecimalSetBytes(&decimal, reinterpret_cast<const uint8_t*>(bytes.data()));
      ArrowCheck(ArrowArrayAppendDecimal(array, &decimal));
      break;
    }
    default: {
      const std::string bytes = field.type == FieldType::kBytes ? Bytes(value) : Text(value);
      ArrowBufferView view{};
      // nanoarrow exposes buffer pointers through its C API union.
      view.data.data = bytes.data();  // NOLINT(cppcoreguidelines-pro-type-union-access)
      view.size_bytes = static_cast<int64_t>(bytes.size());
      ArrowCheck(ArrowArrayAppendBytes(array, view));
      break;
    }
  }
}

bool AvroName(std::string_view name) {
  const auto letter = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  return !name.empty() && letter(name.front()) &&
         std::ranges::all_of(name, [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}

// The names of the Avro records a schema has defined so far. Its RECORD fields are numbered, and
// the record of a RANGE is defined once for each element type, then named.
struct AvroNames {
  int record_id = 0;
  std::set<FieldType> ranges = {};
};

json AvroField(const FieldSchema& field, AvroNames& names) {
  if (field.precision || field.scale) {
    throw ApiError::Invalid("Storage Read does not support parameterized decimals");
  }
  json type;
  switch (field.type) {
    case FieldType::kInteger:
      type = "long";
      break;
    case FieldType::kFloat:
      type = "double";
      break;
    case FieldType::kBoolean:
      type = "boolean";
      break;
    case FieldType::kBytes:
      type = "bytes";
      break;
    case FieldType::kDate:
      type = {{"type", "int"}, {"logicalType", "date"}};
      break;
    case FieldType::kTime:
      type = {{"type", "long"}, {"logicalType", "time-micros"}};
      break;
    case FieldType::kTimestamp:
      type = {{"type", "long"}, {"logicalType", "timestamp-micros"}};
      break;
    case FieldType::kNumeric:
    case FieldType::kBigNumeric:
      type = {
          {"type", "bytes"},
          {"logicalType", "decimal"},
          {"precision", field.type == FieldType::kNumeric ? 38 : 77},
          {"scale", field.type == FieldType::kNumeric ? 9 : 38},
      };
      break;
    case FieldType::kRecord: {
      const std::string name = "__record_" + std::to_string(++names.record_id) + "__";
      json fields = json::array();
      for (const auto& child : field.fields) {
        if (!AvroName(child.name)) {
          throw ApiError::Invalid("Avro field name is not alphanumeric: " + child.name);
        }
        fields.push_back({{"name", child.name}, {"type", AvroField(child, names)}});
      }
      type = {{"type", "record"}, {"name", name}, {"fields", std::move(fields)}};
      break;
    }
    case FieldType::kString:
      type = "string";
      break;
    case FieldType::kDatetime:
      type = {{"type", "string"}, {"logicalType", "datetime"}};
      break;
    case FieldType::kGeography:
    case FieldType::kJson:
      type = {{"type", "string"}, {"sqlType", FieldTypeName(field.type)}};
      break;
    case FieldType::kRange: {
      const FieldType element = RangeBoundField(field, "").type;
      const std::string name = "RANGE_" + std::string(FieldTypeName(element));
      if (!names.ranges.insert(element).second) {
        type = "google.sqlType." + name;
        break;
      }
      json fields = json::array();
      for (const char* bound : {"start", "end"}) {
        fields.push_back(
            {{"name", bound}, {"type", AvroField(RangeBoundField(field, bound), names)}});
      }
      type = {
          {"type", "record"},
          {"name", name},
          {"namespace", "google.sqlType"},
          {"sqlType", "RANGE"},
          {"fields", std::move(fields)},
      };
      break;
    }
    default:
      throw ApiError::Invalid("Storage Read does not support " +
                              std::string(FieldTypeName(field.type)));
  }
  if (field.mode == FieldMode::kRepeated) {
    return {{"type", "array"}, {"items", std::move(type)}};
  }
  if (field.mode == FieldMode::kNullable) {
    return json::array({"null", std::move(type)});
  }
  return type;
}

void AvroLong(std::string& out, int64_t value) {
  uint64_t encoded =
      (static_cast<uint64_t>(value) << 1U) ^ (value < 0 ? std::numeric_limits<uint64_t>::max() : 0);
  while (encoded >= 128) {
    out.push_back(static_cast<char>((encoded & 0x7fU) | 0x80U));
    encoded >>= 7U;
  }
  out.push_back(static_cast<char>(encoded));
}

void AvroValue(std::string& out, const FieldSchema& field, const ArrowArrayView& view,
               int64_t row) {
  if (field.mode == FieldMode::kRepeated) {
    const int64_t begin = ArrowArrayViewListChildOffset(&view, row);
    const int64_t end = ArrowArrayViewListChildOffset(&view, row + 1);
    if (end != begin) {
      AvroLong(out, end - begin);
      FieldSchema item = field;
      item.mode = FieldMode::kRequired;
      for (int64_t i = begin; i < end; ++i) {
        AvroValue(out, item, *view.children[0], i);
      }
    }
    AvroLong(out, 0);
    return;
  }
  if (field.mode == FieldMode::kNullable) {
    const bool null = ArrowArrayViewIsNull(&view, row) != 0;
    AvroLong(out, null ? 0 : 1);
    if (null) {
      return;
    }
  }
  switch (field.type) {
    case FieldType::kRecord:
      for (size_t i = 0; i < field.fields.size(); ++i) {
        AvroValue(out, field.fields.at(i), *view.children[i], row);
      }
      return;
    case FieldType::kRange:
      for (int64_t i = 0; i < 2; ++i) {
        AvroValue(out, RangeBoundField(field, ""), *view.children[i], row);
      }
      return;
    case FieldType::kInteger:
    case FieldType::kDate:
    case FieldType::kTime:
    case FieldType::kTimestamp:
      AvroLong(out, ArrowArrayViewGetIntUnsafe(&view, row));
      return;
    case FieldType::kBoolean:
      out.push_back(ArrowArrayViewGetIntUnsafe(&view, row) != 0 ? '\1' : '\0');
      return;
    case FieldType::kFloat: {
      const auto bits = std::bit_cast<uint64_t>(ArrowArrayViewGetDoubleUnsafe(&view, row));
      for (unsigned i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>(bits >> (8U * i)));
      }
      return;
    }
    case FieldType::kDatetime: {
      googlesql::DatetimeValue datetime;
      Check(googlesql::functions::ConvertTimestampToDatetime(
          absl::FromUnixMicros(ArrowArrayViewGetIntUnsafe(&view, row)), absl::UTCTimeZone(),
          &datetime));
      std::string text;
      Check(googlesql::functions::FormatDatetimeToString("%Y-%m-%d %H:%M:%E*S", datetime, &text));
      AvroLong(out, static_cast<int64_t>(text.size()));
      out += text;
      return;
    }
    case FieldType::kNumeric:
    case FieldType::kBigNumeric: {
      ArrowDecimal decimal{};
      const int bits = field.type == FieldType::kNumeric ? 128 : 256;
      ArrowDecimalInit(&decimal, bits, field.type == FieldType::kNumeric ? 38 : 76,
                       field.type == FieldType::kNumeric ? 9 : 38);
      ArrowArrayViewGetDecimalUnsafe(&view, row, &decimal);
      std::array<uint8_t, 32> bytes{};
      ArrowDecimalGetBytes(&decimal, bytes.data());
      AvroLong(out, bits / 8);
      for (int i = (bits / 8) - 1; i >= 0; --i) {
        out.push_back(static_cast<char>(bytes.at(static_cast<size_t>(i))));
      }
      return;
    }
    default: {
      const ArrowBufferView bytes = ArrowArrayViewGetBytesUnsafe(&view, row);
      AvroLong(out, bytes.size_bytes);
      out.append(bytes.data.as_char,  // NOLINT(cppcoreguidelines-pro-type-union-access)
                 static_cast<size_t>(bytes.size_bytes));
    }
  }
}

}  // namespace

std::string StorageReadSchema(const std::vector<FieldSchema>& schema, bool arrow) {
  if (!arrow) {
    AvroNames names;
    const FieldSchema root{
        .name = "__root__",
        .type = FieldType::kRecord,
        .mode = FieldMode::kRequired,
        .fields = schema,
    };
    json result = AvroField(root, names);
    result["name"] = "__root__";
    return result.dump();
  }
  nanoarrow::UniqueSchema root;
  StorageArrowSchema(root.get(), schema);
  nanoarrow::ipc::UniqueEncoder encoder;
  nanoarrow::UniqueBuffer output;
  ArrowError error{};
  ArrowCheck(ArrowIpcEncoderInit(encoder.get()));
  ArrowCheck(ArrowIpcEncoderEncodeSchema(encoder.get(), root.get(), &error));
  ArrowCheck(ArrowIpcEncoderFinalizeBuffer(encoder.get(), 1, output.get()));
  return {reinterpret_cast<const char*>(output->data), static_cast<size_t>(output->size_bytes)};
}

void StorageArrowSchema(ArrowSchema* out, const std::vector<FieldSchema>& schema) {
  ArrowSchemaInit(out);
  ArrowCheck(ArrowSchemaSetTypeStruct(out, static_cast<int64_t>(schema.size())));
  out->flags =
      static_cast<int64_t>(static_cast<uint64_t>(out->flags) & ~uint64_t{ARROW_FLAG_NULLABLE});
  for (size_t i = 0; i < schema.size(); ++i) {
    ArrowField(out->children[i], schema.at(i));
  }
}

void AppendArrowCell(ArrowArray* array, const FieldSchema& field, const json& value) {
  ArrowCell(array, field, value);
}

std::string EncodeAvroRows(const ArrowSchema& root, const ArrowArray& array,
                           const std::vector<FieldSchema>& schema) {
  nanoarrow::UniqueArrayView view;
  ArrowError error{};
  ArrowCheck(ArrowArrayViewInitFromSchema(view.get(), &root, &error));
  ArrowCheck(ArrowArrayViewSetArray(view.get(), &array, &error));
  std::string out;
  for (int64_t row = 0; row < array.length; ++row) {
    for (size_t column = 0; column < schema.size(); ++column) {
      AvroValue(out, schema.at(column), *view->children[column], row);
    }
  }
  return out;
}

std::string EncodeArrowRecordBatch(const ArrowSchema& root, const ArrowArray& array) {
  nanoarrow::UniqueArrayView view;
  ArrowError error{};
  ArrowCheck(ArrowArrayViewInitFromSchema(view.get(), &root, &error));
  ArrowCheck(ArrowArrayViewSetArray(view.get(), &array, &error));
  nanoarrow::ipc::UniqueEncoder encoder;
  nanoarrow::UniqueBuffer header;
  nanoarrow::UniqueBuffer body;
  ArrowCheck(ArrowIpcEncoderInit(encoder.get()));
  ArrowCheck(ArrowIpcEncoderEncodeSimpleRecordBatch(encoder.get(), view.get(), body.get(), &error));
  ArrowCheck(ArrowIpcEncoderFinalizeBuffer(encoder.get(), 1, header.get()));
  std::string output(reinterpret_cast<const char*>(header->data),
                     static_cast<size_t>(header->size_bytes));
  if (body->size_bytes != 0) {
    output.append(reinterpret_cast<const char*>(body->data), static_cast<size_t>(body->size_bytes));
  }
  return output;
}

}  // namespace bigquery_emulator_duckdb
