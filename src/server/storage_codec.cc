#include "src/server/storage_codec.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/time/time.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/message.h"
#include "google/protobuf/unknown_field_set.h"
#include "googlesql/common/utf_util.h"
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

using google::protobuf::Descriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
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
    default:
      throw ApiError::Invalid("Storage Read does not support " +
                              std::string(FieldTypeName(field.type)));
  }
}

nanoarrow::UniqueSchema ArrowRoot(const std::vector<FieldSchema>& fields) {
  nanoarrow::UniqueSchema out;
  ArrowSchemaInit(out.get());
  ArrowCheck(ArrowSchemaSetTypeStruct(out.get(), static_cast<int64_t>(fields.size())));
  out->flags =
      static_cast<int64_t>(static_cast<uint64_t>(out->flags) & ~uint64_t{ARROW_FLAG_NULLABLE});
  for (size_t i = 0; i < fields.size(); ++i) {
    ArrowField(out->children[i], fields.at(i));
  }
  return out;
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

json AvroField(const FieldSchema& field, int& record_id) {
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
      const std::string name = "__record_" + std::to_string(++record_id) + "__";
      json fields = json::array();
      for (const auto& child : field.fields) {
        if (!AvroName(child.name)) {
          throw ApiError::Invalid("Avro field name is not alphanumeric: " + child.name);
        }
        fields.push_back({{"name", child.name}, {"type", AvroField(child, record_id)}});
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

void AvroCell(std::string& out, const FieldSchema& field, const json& value) {
  if (field.mode == FieldMode::kRepeated) {
    if (!value.empty()) {
      AvroLong(out, static_cast<int64_t>(value.size()));
      FieldSchema item = field;
      item.mode = FieldMode::kRequired;
      for (const auto& cell : value) {
        AvroCell(out, item, cell.at("v"));
      }
    }
    AvroLong(out, 0);
    return;
  }
  if (field.mode == FieldMode::kNullable) {
    AvroLong(out, value.is_null() ? 0 : 1);
    if (value.is_null()) {
      return;
    }
  }
  switch (field.type) {
    case FieldType::kRecord:
      for (size_t i = 0; i < field.fields.size(); ++i) {
        AvroCell(out, field.fields.at(i), value.at("f").at(i).at("v"));
      }
      return;
    case FieldType::kInteger:
      AvroLong(out, std::stoll(Text(value)));
      return;
    case FieldType::kBoolean:
      out.push_back(Text(value) == "true" ? '\1' : '\0');
      return;
    case FieldType::kFloat: {
      const auto bits = std::bit_cast<uint64_t>(std::stod(Text(value)));
      for (unsigned i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>(bits >> (8U * i)));
      }
      return;
    }
    case FieldType::kDate:
    case FieldType::kTime:
    case FieldType::kTimestamp:
      AvroLong(out, Temporal(field, value));
      return;
    default: {
      std::string text;
      if (field.type == FieldType::kBytes) {
        text = Bytes(value);
      } else if (field.type == FieldType::kNumeric || field.type == FieldType::kBigNumeric) {
        text = DecimalBytes(field, value);
        std::ranges::reverse(text);
      } else {
        text = Text(value);
        if (field.type == FieldType::kDatetime && text.size() > 10) {
          text.at(10) = ' ';
        }
      }
      AvroLong(out, static_cast<int64_t>(text.size()));
      out += text;
    }
  }
}

const FieldSchema& Destination(const std::vector<FieldSchema>& schema, std::string_view name) {
  const auto found = std::ranges::find(schema, name, &FieldSchema::name);
  if (found == schema.end()) {
    throw ApiError::Invalid("Writer schema contains unknown field: " + std::string(name));
  }
  return *found;
}

bool ProtoType(const FieldSchema& field, const FieldDescriptor& descriptor) {
  const auto type = descriptor.cpp_type();
  const bool integer = type == FieldDescriptor::CPPTYPE_INT32 ||
                       type == FieldDescriptor::CPPTYPE_INT64 ||
                       type == FieldDescriptor::CPPTYPE_UINT32;
  const bool string = descriptor.type() == FieldDescriptor::TYPE_STRING;
  const bool bytes = descriptor.type() == FieldDescriptor::TYPE_BYTES;
  switch (field.type) {
    case FieldType::kInteger:
      return integer || type == FieldDescriptor::CPPTYPE_ENUM;
    case FieldType::kFloat:
      return type == FieldDescriptor::CPPTYPE_FLOAT || type == FieldDescriptor::CPPTYPE_DOUBLE;
    case FieldType::kBoolean:
      return type == FieldDescriptor::CPPTYPE_BOOL;
    case FieldType::kBytes:
      return bytes;
    case FieldType::kNumeric:
    case FieldType::kBigNumeric:
      return bytes || string;
    case FieldType::kDate:
      return descriptor.type() == FieldDescriptor::TYPE_INT32 || string;
    case FieldType::kTime:
    case FieldType::kDatetime:
      return descriptor.type() == FieldDescriptor::TYPE_INT64 || string;
    case FieldType::kTimestamp:
      return descriptor.type() == FieldDescriptor::TYPE_INT64;
    case FieldType::kRecord:
      return type == FieldDescriptor::CPPTYPE_MESSAGE;
    case FieldType::kString:
    case FieldType::kGeography:
    case FieldType::kJson:
      return string;
    default:
      return false;
  }
}

void ValidateDefaults(const std::vector<FieldSchema>& schema) {
  for (const auto& field : schema) {
    if (!field.default_value_expression.empty()) {
      throw ApiError::Invalid("Storage Write does not support column defaults");
    }
    if (field.max_length || field.precision || field.scale) {
      throw ApiError::Invalid("Storage Write does not support parameterized columns");
    }
    if (field.type == FieldType::kRecord) {
      ValidateDefaults(field.fields);
    }
  }
}

void ValidateProto(const Descriptor& descriptor, const std::vector<FieldSchema>& schema,
                   int depth) {
  if (depth > 15) {
    throw ApiError::Invalid("Writer schema exceeds maximum nesting depth");
  }
  ValidateDefaults(schema);
  for (int i = 0; i < descriptor.field_count(); ++i) {
    const auto& input = *descriptor.field(i);
    const auto& field = Destination(schema, input.name());
    if (input.containing_oneof() != nullptr || input.is_map()) {
      throw ApiError::Invalid("Storage Write does not support oneof or map fields");
    }
    std::vector<const FieldDescriptor*> options;
    google::protobuf::FieldOptions::GetReflection()->ListFields(input.options(), &options);
    if (input.options().uninterpreted_option_size() != 0 ||
        !google::protobuf::FieldOptions::GetReflection()
             ->GetUnknownFields(input.options())
             .empty() ||
        std::ranges::any_of(options, [](const auto* option) { return option->is_extension(); })) {
      throw ApiError::Invalid("Storage Write does not support protobuf field annotations");
    }
    if (input.is_repeated() != (field.mode == FieldMode::kRepeated) || !ProtoType(field, input)) {
      throw ApiError::Invalid("Writer schema type does not match field: " + field.name);
    }
    if (field.type == FieldType::kRecord) {
      ValidateProto(*input.message_type(), field.fields, depth + 1);
    }
  }
}

json ProtoMessage(const Message& message, const std::vector<FieldSchema>& schema);

json ProtoValue(const Message& message, const FieldDescriptor& input, const FieldSchema& field,
                int index) {
  const auto* r = message.GetReflection();
  const bool repeated = index >= 0;
  if (input.cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
    return ProtoMessage(
        repeated ? r->GetRepeatedMessage(message, &input, index) : r->GetMessage(message, &input),
        field.fields);
  }
  if (input.cpp_type() == FieldDescriptor::CPPTYPE_STRING) {
    std::string text =
        repeated ? r->GetRepeatedString(message, &input, index) : r->GetString(message, &input);
    if (input.type() != FieldDescriptor::TYPE_BYTES) {
      if (!googlesql::IsWellFormedUTF8(text)) {
        throw ApiError::Invalid("Invalid UTF-8: " + field.name);
      }
      std::string canonical;
      switch (field.type) {
        case FieldType::kDate: {
          int32_t date = 0;
          Check(googlesql::functions::ConvertStringToDate(text, &date));
          Check(googlesql::functions::ConvertDateToString(date, &canonical));
          return canonical;
        }
        case FieldType::kTime: {
          googlesql::TimeValue time;
          Check(googlesql::functions::ConvertStringToTime(text, googlesql::functions::kMicroseconds,
                                                          &time));
          return time.DebugString();
        }
        case FieldType::kDatetime: {
          googlesql::DatetimeValue datetime;
          Check(googlesql::functions::ConvertStringToDatetime(
              text, googlesql::functions::kMicroseconds, &datetime));
          return datetime.DebugString();
        }
        case FieldType::kTimestamp: {
          int64_t timestamp = 0;
          Check(googlesql::functions::ConvertStringToTimestamp(
              text, absl::UTCTimeZone(), googlesql::functions::kMicroseconds, true, &timestamp));
          Check(googlesql::functions::ConvertTimestampToStringWithTruncation(
              timestamp, googlesql::functions::kMicroseconds, absl::UTCTimeZone(), &canonical));
          return canonical;
        }
        case FieldType::kJson:
        case FieldType::kGeography:
          throw ApiError::Invalid("Storage Write does not support JSON or GEOGRAPHY input");
        case FieldType::kNumeric: {
          const auto value = googlesql::NumericValue::FromStringStrict(text);
          Check(value.status());
          return value->ToString();
        }
        case FieldType::kBigNumeric: {
          const auto value = googlesql::BigNumericValue::FromStringStrict(text);
          Check(value.status());
          return value->ToString();
        }
        default:
          break;
      }
      return text;
    }
    if (field.type == FieldType::kBytes) {
      return absl::Base64Escape(text);
    }
    if (field.type == FieldType::kNumeric) {
      const auto value = googlesql::NumericValue::DeserializeFromProtoBytes(text);
      Check(value.status());
      return value->ToString();
    }
    const auto value = googlesql::BigNumericValue::DeserializeFromProtoBytes(text);
    Check(value.status());
    return value->ToString();
  }
  if (input.cpp_type() == FieldDescriptor::CPPTYPE_BOOL) {
    return repeated ? r->GetRepeatedBool(message, &input, index) : r->GetBool(message, &input);
  }
  if (input.cpp_type() == FieldDescriptor::CPPTYPE_DOUBLE ||
      input.cpp_type() == FieldDescriptor::CPPTYPE_FLOAT) {
    const double value = input.cpp_type() == FieldDescriptor::CPPTYPE_DOUBLE
                             ? (repeated ? r->GetRepeatedDouble(message, &input, index)
                                         : r->GetDouble(message, &input))
                             : (repeated ? r->GetRepeatedFloat(message, &input, index)
                                         : r->GetFloat(message, &input));
    if (std::isnan(value)) {
      return "NaN";
    }
    if (std::isinf(value)) {
      return value < 0 ? "-Infinity" : "Infinity";
    }
    return value;
  }
  int64_t value = 0;
  switch (input.cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
      value = repeated ? r->GetRepeatedInt32(message, &input, index) : r->GetInt32(message, &input);
      break;
    case FieldDescriptor::CPPTYPE_INT64:
      value = repeated ? r->GetRepeatedInt64(message, &input, index) : r->GetInt64(message, &input);
      break;
    case FieldDescriptor::CPPTYPE_UINT32:
      value =
          repeated ? r->GetRepeatedUInt32(message, &input, index) : r->GetUInt32(message, &input);
      break;
    case FieldDescriptor::CPPTYPE_UINT64: {
      const uint64_t number =
          repeated ? r->GetRepeatedUInt64(message, &input, index) : r->GetUInt64(message, &input);
      if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw ApiError::Invalid("Value exceeds INT64 range: " + field.name);
      }
      value = static_cast<int64_t>(number);
      break;
    }
    case FieldDescriptor::CPPTYPE_ENUM:
      value = repeated ? r->GetRepeatedEnumValue(message, &input, index)
                       : r->GetEnumValue(message, &input);
      break;
    default:
      throw ApiError::Invalid("Unsupported protobuf field: " + field.name);
  }
  std::string text;
  switch (field.type) {
    case FieldType::kDate:
      Check(googlesql::functions::ConvertDateToString(static_cast<int32_t>(value), &text));
      return text;
    case FieldType::kTimestamp:
      Check(googlesql::functions::ConvertTimestampToStringWithTruncation(
          value, googlesql::functions::kMicroseconds, absl::UTCTimeZone(), &text));
      return text;
    case FieldType::kTime: {
      const auto time = googlesql::TimeValue::FromPacked64Micros(value);
      if (!time.IsValid()) {
        throw ApiError::Invalid("Invalid packed TIME: " + field.name);
      }
      return time.DebugString();
    }
    case FieldType::kDatetime: {
      const auto datetime = googlesql::DatetimeValue::FromPacked64Micros(value);
      if (!datetime.IsValid()) {
        throw ApiError::Invalid("Invalid packed DATETIME: " + field.name);
      }
      return datetime.DebugString();
    }
    default:
      return std::to_string(value);
  }
}

json ProtoMessage(const Message& message, const std::vector<FieldSchema>& schema) {
  const auto* descriptor = message.GetDescriptor();
  const auto* reflection = message.GetReflection();
  if (!reflection->GetUnknownFields(message).empty()) {
    throw ApiError::Invalid("Serialized row contains unknown protobuf fields");
  }
  json result = json::object();
  for (const auto& field : schema) {
    const auto* found = descriptor->FindFieldByName(field.name);
    if (found == nullptr) {
      if (field.mode == FieldMode::kRequired) {
        throw ApiError::Invalid("Missing required field: " + field.name);
      }
      result[field.name] = field.mode == FieldMode::kRepeated ? json::array() : json(nullptr);
      continue;
    }
    const auto& input = *found;
    if (input.is_repeated()) {
      json values = json::array();
      for (int j = 0; j < reflection->FieldSize(message, &input); ++j) {
        values.push_back(ProtoValue(message, input, field, j));
      }
      result[field.name] = std::move(values);
    } else if (reflection->HasField(message, &input) || input.has_default_value()) {
      result[field.name] = ProtoValue(message, input, field, -1);
    } else {
      if (field.mode == FieldMode::kRequired) {
        throw ApiError::Invalid("Missing required field: " + field.name);
      }
      result[field.name] = nullptr;
    }
  }
  return result;
}

}  // namespace

std::string StorageReadSchema(const std::vector<FieldSchema>& schema, bool arrow) {
  if (!arrow) {
    int id = 0;
    const FieldSchema root{
        .name = "__root__",
        .type = FieldType::kRecord,
        .mode = FieldMode::kRequired,
        .fields = schema,
    };
    json result = AvroField(root, id);
    result["name"] = "__root__";
    return result.dump();
  }
  const auto root = ArrowRoot(schema);
  nanoarrow::ipc::UniqueEncoder encoder;
  nanoarrow::UniqueBuffer output;
  ArrowError error{};
  ArrowCheck(ArrowIpcEncoderInit(encoder.get()));
  ArrowCheck(ArrowIpcEncoderEncodeSchema(encoder.get(), root.get(), &error));
  ArrowCheck(ArrowIpcEncoderFinalizeBuffer(encoder.get(), 1, output.get()));
  return {reinterpret_cast<const char*>(output->data), static_cast<size_t>(output->size_bytes)};
}

std::string StorageReadRows(const std::vector<FieldSchema>& schema, std::span<const json> rows,
                            bool arrow) {
  if (!arrow) {
    std::string output;
    for (const auto& row : rows) {
      for (size_t i = 0; i < schema.size(); ++i) {
        AvroCell(output, schema.at(i), row.at("f").at(i).at("v"));
      }
    }
    return output;
  }
  const auto root = ArrowRoot(schema);
  nanoarrow::UniqueArray array;
  nanoarrow::UniqueArrayView view;
  ArrowError error{};
  ArrowCheck(ArrowArrayInitFromSchema(array.get(), root.get(), &error));
  ArrowCheck(ArrowArrayStartAppending(array.get()));
  for (const auto& row : rows) {
    for (size_t i = 0; i < schema.size(); ++i) {
      ArrowCell(array->children[i], schema.at(i), row.at("f").at(i).at("v"));
    }
    ArrowCheck(ArrowArrayFinishElement(array.get()));
  }
  ArrowCheck(ArrowArrayFinishBuildingDefault(array.get(), &error));
  ArrowCheck(ArrowArrayViewInitFromSchema(view.get(), root.get(), &error));
  ArrowCheck(ArrowArrayViewSetArray(view.get(), array.get(), &error));
  nanoarrow::ipc::UniqueEncoder encoder;
  nanoarrow::UniqueBuffer header;
  nanoarrow::UniqueBuffer body;
  ArrowCheck(ArrowIpcEncoderInit(encoder.get()));
  ArrowCheck(ArrowIpcEncoderEncodeSimpleRecordBatch(encoder.get(), view.get(), body.get(), &error));
  ArrowCheck(ArrowIpcEncoderFinalizeBuffer(encoder.get(), 1, header.get()));
  std::string output(reinterpret_cast<const char*>(header->data),
                     static_cast<size_t>(header->size_bytes));
  output.append(reinterpret_cast<const char*>(body->data), static_cast<size_t>(body->size_bytes));
  return output;
}

class StorageProtoDecoder::Impl {
 public:
  explicit Impl(const google::protobuf::DescriptorProto& descriptor,
                const std::vector<FieldSchema>& fields)
      : schema(fields), factory(&pool) {
    google::protobuf::FileDescriptorProto file;
    file.set_name("storage_writer.proto");
    file.set_syntax("proto2");
    *file.add_message_type() = descriptor;
    const auto* built = pool.BuildFile(file);
    if (built == nullptr || built->message_type_count() != 1) {
      throw ApiError::Invalid("Invalid self-contained writer schema");
    }
    ValidateProto(*built->message_type(0), schema, 0);
    prototype = factory.GetPrototype(built->message_type(0));
  }
  std::vector<FieldSchema> schema;
  google::protobuf::DescriptorPool pool;
  google::protobuf::DynamicMessageFactory factory;
  const Message* prototype = nullptr;
};

StorageProtoDecoder::StorageProtoDecoder(const google::protobuf::DescriptorProto& descriptor,
                                         const std::vector<FieldSchema>& schema)
    : impl_(std::make_unique<Impl>(descriptor, schema)) {}

StorageProtoDecoder::~StorageProtoDecoder() = default;

json StorageProtoDecoder::Decode(const std::string& bytes) const {
  const std::unique_ptr<Message> message(impl_->prototype->New());
  if (!message->ParseFromString(bytes)) {
    throw ApiError::Invalid("Invalid serialized protobuf row");
  }
  return ProtoMessage(*message, impl_->schema);
}

}  // namespace bigquery_emulator_duckdb
