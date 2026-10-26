#include "src/server/storage_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
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
    if (field.max_length) {
      throw ApiError::Invalid(
          "Storage Write does not support parameterized STRING or BYTES columns");
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
    if (field.type == FieldType::kJson || field.type == FieldType::kInterval) {
      throw ApiError::Invalid("Storage Write does not support JSON or INTERVAL input");
    }
    if (!ProtoType(field, input)) {
      throw StorageFieldMismatch(std::string(input.full_name()), std::string(input.type_name()),
                                 field.type);
    }
    if (input.is_repeated() != (field.mode == FieldMode::kRepeated)) {
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
