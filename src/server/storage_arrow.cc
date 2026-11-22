#include "src/server/storage_arrow.h"

#include <algorithm>
#include <cmath>
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
#include "googlesql/public/interval_value.h"
#include "googlesql/public/numeric_value.h"
#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow.hpp"
#include "nanoarrow/nanoarrow_ipc.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/field_schema.h"
#include "src/server/storage_codec.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;
namespace time_util = googlesql::functions;

void Check(int code, const ArrowError& error) {
  if (code != NANOARROW_OK) {
    throw ApiError::Invalid("Invalid Arrow IPC message: " + std::string(error.message));
  }
}

void Check(const absl::Status& status) {
  if (!status.ok()) {
    throw ApiError::Invalid(std::string(status.message()));
  }
}

ArrowBufferView Buffer(std::string_view bytes) {
  ArrowBufferView view{};
  view.data.data = bytes.data();  // NOLINT(cppcoreguidelines-pro-type-union-access)
  view.size_bytes = static_cast<int64_t>(bytes.size());
  return view;
}

std::string_view DecodeMessage(ArrowIpcDecoder* decoder, std::string_view bytes,
                               ArrowIpcMessageType type, ArrowError& error) {
  Check(ArrowIpcDecoderVerifyHeader(decoder, Buffer(bytes), &error), error);
  if (decoder->message_type != type || decoder->header_size_bytes < 0 ||
      std::cmp_greater(decoder->header_size_bytes, bytes.size())) {
    throw ApiError::Invalid("Unexpected Arrow IPC message header");
  }
  const auto body = bytes.substr(static_cast<size_t>(decoder->header_size_bytes));
  if (std::cmp_not_equal(decoder->body_size_bytes, body.size())) {
    throw ApiError::Invalid("Arrow IPC message length does not match its header");
  }
  Check(ArrowIpcDecoderDecodeHeader(decoder, Buffer(bytes), &error), error);
  return body;
}

bool IsUnsigned(ArrowType type) {
  return type == NANOARROW_TYPE_UINT8 || type == NANOARROW_TYPE_UINT16 ||
         type == NANOARROW_TYPE_UINT32 || type == NANOARROW_TYPE_UINT64;
}

// Storage Write's Arrow mappings (third_party/bigquery_docs/supported-data-types.md).
bool Matches(const ArrowSchemaView& view, FieldType type) {
  switch (type) {
    case FieldType::kInteger:
      return view.type == NANOARROW_TYPE_INT64 || IsUnsigned(view.type);
    case FieldType::kString:
    case FieldType::kJson:
    case FieldType::kGeography:
      return view.type == NANOARROW_TYPE_STRING;
    case FieldType::kBoolean:
      return view.type == NANOARROW_TYPE_BOOL;
    case FieldType::kBytes:
      return view.type == NANOARROW_TYPE_BINARY;
    case FieldType::kFloat:
      return view.type == NANOARROW_TYPE_FLOAT || view.type == NANOARROW_TYPE_DOUBLE;
    case FieldType::kDate:
      return view.type == NANOARROW_TYPE_DATE32 || view.type == NANOARROW_TYPE_INT32 ||
             view.type == NANOARROW_TYPE_STRING;
    case FieldType::kDatetime:
    case FieldType::kTimestamp:
      return view.type == NANOARROW_TYPE_TIMESTAMP && view.time_unit == NANOARROW_TIME_UNIT_MICRO &&
             std::string_view(view.timezone) == (type == FieldType::kTimestamp ? "UTC" : "");
    case FieldType::kNumeric:
    case FieldType::kBigNumeric: {
      const bool wide = type == FieldType::kBigNumeric;
      return view.type == (wide ? NANOARROW_TYPE_DECIMAL256 : NANOARROW_TYPE_DECIMAL128) &&
             view.decimal_precision > 0 && view.decimal_precision <= (wide ? 76 : 38) &&
             view.decimal_scale >= 0 && view.decimal_scale <= (wide ? 38 : 9) &&
             view.decimal_scale <= view.decimal_precision;
    }
    case FieldType::kInterval:
      return view.type == NANOARROW_TYPE_INTERVAL_MONTHS ||
             view.type == NANOARROW_TYPE_INTERVAL_DAY_TIME ||
             view.type == NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO ||
             view.type == NANOARROW_TYPE_STRING;
    case FieldType::kRecord:
    case FieldType::kRange:
      return view.type == NANOARROW_TYPE_STRUCT;
    case FieldType::kTime:
      // Accepted by BigQuery despite its omission from the Storage Write type table.
      return view.type == NANOARROW_TYPE_TIME64 && view.time_unit == NANOARROW_TIME_UNIT_MICRO;
  }
  return false;
}

size_t DataAlignment(ArrowType type) {
  switch (type) {
    case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_UINT64:
      return alignof(int64_t);
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LIST:
      return alignof(int32_t);
    case NANOARROW_TYPE_UINT16:
      return alignof(uint16_t);
    case NANOARROW_TYPE_FLOAT:
      return alignof(float);
    case NANOARROW_TYPE_DOUBLE:
      return alignof(double);
    default:
      return 1;  // The decimal and interval accessors copy bytes.
  }
}

// Check all lengths, buffer sizes, and alignment before nanoarrow multiplies lengths or
// dereferences typed offsets (including for empty arrays with a nonempty offset buffer).
void CheckLayout(const ArrowArrayView& col, int64_t body_size, int64_t& remaining) {
  const auto length = col.length;
  const auto minimum_bytes = (length / 8) + (length % 8 != 0 ? 1 : 0);
  // All accepted trees have either an offset or a leaf with at least one bit per row.
  if (length < 0 || minimum_bytes > body_size ||
      length >= std::numeric_limits<int64_t>::max() / 256 || col.offset != 0 ||
      col.null_count < 0 || col.null_count > length) {
    throw ApiError::Invalid("Invalid Arrow array length or null count");
  }
  for (const auto& buffer : col.buffer_views) {
    if (buffer.size_bytes < 0 || buffer.size_bytes > remaining) {
      throw ApiError::Invalid("Arrow column buffers exceed the batch body");
    }
    remaining -= buffer.size_bytes;
  }
  const auto& data = col.buffer_views[1];
  const auto address = reinterpret_cast<uintptr_t>(
      data.data.data);  // NOLINT(cppcoreguidelines-pro-type-union-access)
  if (address % DataAlignment(col.storage_type) != 0 ||
      (col.layout.buffer_type[1] == NANOARROW_BUFFER_TYPE_DATA_OFFSET && data.size_bytes > 0 &&
       std::cmp_less(data.size_bytes, sizeof(int32_t)))) {
    throw ApiError::Invalid("Invalid Arrow buffer length or alignment");
  }
  for (int64_t i = 0; i < col.n_children; ++i) {
    const auto& child = *col.children[i];
    if (col.storage_type == NANOARROW_TYPE_STRUCT && child.length != length) {
      throw ApiError::Invalid("Arrow struct child length does not match its parent");
    }
    CheckLayout(child, body_size, remaining);
  }
}

void CheckNullCounts(const ArrowArrayView& col) {
  if (col.null_count != ArrowArrayViewComputeNullCount(&col)) {
    throw ApiError::Invalid("Arrow null count does not match the validity bitmap");
  }
  for (int64_t i = 0; i < col.n_children; ++i) {
    CheckNullCounts(*col.children[i]);
  }
}

std::string_view Bytes(const ArrowArrayView* col, int64_t row) {
  if (col->buffer_views[2].size_bytes == 0) {
    return "";
  }
  const auto bytes = ArrowArrayViewGetStringUnsafe(col, row);
  return {bytes.data, static_cast<size_t>(bytes.size_bytes)};
}

std::string Temporal(FieldType type, int64_t value) {
  std::string text;
  switch (type) {
    case FieldType::kDate:
      Check(time_util::ConvertDateToString(static_cast<int32_t>(value), &text));
      return text;
    case FieldType::kTime: {
      if (value < 0 || absl::Microseconds(value) >= absl::Hours(24)) {
        throw ApiError::Invalid("Arrow Time64 value is outside the TIME range");
      }
      googlesql::TimeValue time;
      Check(time_util::ConvertTimestampToTime(absl::FromUnixMicros(value), absl::UTCTimeZone(),
                                              time_util::kMicroseconds, &time));
      return time.DebugString();
    }
    case FieldType::kDatetime: {
      googlesql::DatetimeValue datetime;
      Check(time_util::ConvertTimestampToDatetime(absl::FromUnixMicros(value), absl::UTCTimeZone(),
                                                  &datetime));
      return datetime.DebugString();
    }
    default:
      Check(time_util::ConvertTimestampToStringWithTruncation(value, time_util::kMicroseconds,
                                                              absl::UTCTimeZone(), &text));
      return text;
  }
}

std::string Interval(const ArrowArrayView* col, int64_t row, std::string_view name) {
  if (col->storage_type == NANOARROW_TYPE_STRING) {
    const auto value = googlesql::IntervalValue::Parse(
        DecodeStorageText(Bytes(col, row), FieldType::kInterval, name), /*allow_nanos=*/false);
    Check(value.status());
    return value->ToString();
  }
  ArrowInterval interval{};
  ArrowIntervalInit(&interval, col->storage_type);
  ArrowArrayViewGetIntervalUnsafe(col, row, &interval);
  using googlesql::IntervalValue;
  const __int128 nanos = (interval.ms * IntervalValue::kNanosInMilli) + interval.ns;
  const auto value = IntervalValue::FromMonthsDaysNanos(interval.months, interval.days, nanos);
  Check(value.status());
  return value->ToString();
}

}  // namespace

std::string StorageArrowDecoder::DecodeDecimal(const ArrowArrayView* col, int64_t row,
                                               const Field& field) {
  const bool wide = col->storage_type == NANOARROW_TYPE_DECIMAL256;
  ArrowDecimal decimal{};
  ArrowDecimalInit(&decimal, wide ? 256 : 128, field.precision, field.scale);
  ArrowArrayViewGetDecimalUnsafe(col, row, &decimal);
  nanoarrow::UniqueBuffer buffer;
  const ArrowError error{};
  Check(ArrowDecimalAppendStringToBuffer(&decimal, buffer.get()), error);
  const std::string_view text(reinterpret_cast<const char*>(buffer->data),
                              static_cast<size_t>(buffer->size_bytes));
  auto integer = text.substr(text.starts_with('-') ? 1 : 0);
  integer = integer.substr(0, integer.find('.'));
  if (integer != "0" && std::cmp_greater(integer.size(), field.precision - field.scale)) {
    throw ApiError::Invalid("Arrow decimal exceeds its declared precision");
  }
  if (wide) {
    const auto value = googlesql::BigNumericValue::FromStringStrict(text);
    Check(value.status());
    return value->ToString();
  }
  const auto value = googlesql::NumericValue::FromStringStrict(text);
  Check(value.status());
  return value->ToString();
}

StorageArrowDecoder::Field StorageArrowDecoder::Bind(const ArrowSchema& input,
                                                     const FieldSchema& destination, int depth) {
  // Each BigQuery nesting level can add a list and a struct.
  if (depth > 30) {
    throw ApiError::Invalid("Arrow writer schema exceeds maximum nesting depth");
  }
  if (input.dictionary != nullptr || input.metadata != nullptr) {
    throw ApiError::Unsupported("Arrow writer field encoding or metadata: " + destination.name);
  }
  const bool repeated = destination.mode == FieldMode::kRepeated;
  Field field{.name = destination.name, .type = destination.type, .repeated = repeated};
  ArrowSchemaView view{};
  ArrowError error{};
  Check(ArrowSchemaViewInit(&view, &input, &error), error);
  if (repeated && view.type == NANOARROW_TYPE_LIST) {
    field.children.push_back(Bind(*input.children[0], AsNullable(destination), depth + 1));
    return field;
  }
  if (repeated || !Matches(view, destination.type)) {
    throw ApiError::Invalid("Arrow writer schema type does not match field: " + destination.name);
  }
  switch (destination.type) {
    case FieldType::kNumeric:
    case FieldType::kBigNumeric:
      field.precision = view.decimal_precision;
      field.scale = view.decimal_scale;
      break;
    case FieldType::kRecord:
      if (input.n_children == 0) {
        throw ApiError::Unsupported("empty Arrow writer structs");
      }
      field.children = BindFields(input, destination.fields, depth + 1);
      break;
    case FieldType::kRange: {
      if (input.n_children != 2) {
        throw ApiError::Invalid("Arrow range struct requires start and end fields");
      }
      const auto start = RangeBoundField(destination, "start");
      const auto end = RangeBoundField(destination, "end");
      field.children = BindFields(input, {start, end}, depth + 1);
      if (start.type == FieldType::kDate) {
        for (int64_t i = 0; i < input.n_children; ++i) {
          if (std::string_view(input.children[i]->format) != "tdD") {
            throw ApiError::Invalid("Arrow RANGE<DATE> requires Date32 bounds");
          }
        }
      }
      break;
    }
    default:
      break;
  }
  return field;
}

std::vector<StorageArrowDecoder::Field> StorageArrowDecoder::BindFields(
    const ArrowSchema& input, const std::vector<FieldSchema>& destination, int depth) {
  std::vector<Field> fields;
  std::set<std::string_view> seen;
  for (int64_t i = 0; i < input.n_children; ++i) {
    const auto& child = *input.children[i];
    const auto found =
        std::ranges::find(destination, std::string_view(child.name), &FieldSchema::name);
    if (found == destination.end() || !seen.insert(child.name).second) {
      throw ApiError::Invalid("Arrow writer schema has an unknown or duplicate field: " +
                              std::string(child.name));
    }
    fields.push_back(Bind(child, *found, depth));
  }
  return fields;
}

json StorageArrowDecoder::DecodeStruct(const ArrowArrayView* col, int64_t row,
                                       const std::vector<Field>& fields) {
  auto values = json::object();
  for (int64_t i = 0; i < col->n_children; ++i) {
    const auto& field = fields.at(static_cast<size_t>(i));
    values[field.name] = DecodeValue(col->children[i], row, field);
  }
  return values;
}

json StorageArrowDecoder::DecodeValue(const ArrowArrayView* col, int64_t row, const Field& field) {
  const bool is_null = ArrowArrayViewIsNull(col, row) != 0;
  if (field.repeated) {
    if (is_null) {
      throw ApiError::Invalid("Arrow repeated field must use an empty list for NULL: " +
                              field.name);
    }
    auto values = json::array();
    const auto begin = ArrowArrayViewListChildOffset(col, row);
    const auto end = ArrowArrayViewListChildOffset(col, row + 1);
    for (auto i = begin; i < end; ++i) {
      auto value = DecodeValue(col->children[0], i, field.children.front());
      if (value.is_null()) {
        throw ApiError::Invalid("Arrow repeated field cannot contain NULL elements: " + field.name);
      }
      values.push_back(std::move(value));
    }
    return values;
  }
  if (is_null) {
    return nullptr;
  }
  switch (field.type) {
    case FieldType::kInteger:
      if (IsUnsigned(col->storage_type)) {
        const auto value = ArrowArrayViewGetUIntUnsafe(col, row);
        if (std::cmp_greater(value, std::numeric_limits<int64_t>::max())) {
          throw ApiError::Invalid("Arrow integer exceeds INT64 range: " + field.name);
        }
        return std::to_string(value);
      }
      return std::to_string(ArrowArrayViewGetIntUnsafe(col, row));
    case FieldType::kBoolean:
      return ArrowArrayViewGetIntUnsafe(col, row) != 0;
    case FieldType::kFloat: {
      const double value = ArrowArrayViewGetDoubleUnsafe(col, row);
      if (std::isnan(value)) {
        return "NaN";
      }
      if (std::isinf(value)) {
        return value < 0 ? "-Infinity" : "Infinity";
      }
      return value;
    }
    case FieldType::kBytes:
      return absl::Base64Escape(Bytes(col, row));
    case FieldType::kString:
    case FieldType::kGeography:
    case FieldType::kJson:
      return DecodeStorageText(Bytes(col, row), field.type, field.name);
    case FieldType::kDate:
      if (col->storage_type == NANOARROW_TYPE_STRING) {
        return DecodeStorageText(Bytes(col, row), field.type, field.name);
      }
      return Temporal(field.type, ArrowArrayViewGetIntUnsafe(col, row));
    case FieldType::kTime:
    case FieldType::kDatetime:
    case FieldType::kTimestamp:
      return Temporal(field.type, ArrowArrayViewGetIntUnsafe(col, row));
    case FieldType::kNumeric:
    case FieldType::kBigNumeric:
      return DecodeDecimal(col, row, field);
    case FieldType::kInterval:
      return Interval(col, row, field.name);
    case FieldType::kRange:
    case FieldType::kRecord:
      return DecodeStruct(col, row, field.children);
  }
  throw ApiError::Unsupported("Arrow writer array type");
}

StorageArrowDecoder::StorageArrowDecoder(std::string_view wire_schema,
                                         const std::vector<FieldSchema>& destination) {
  ValidateStorageWriteSchema(destination);
  ArrowError error{};
  auto* decoder = decoder_.get();
  Check(ArrowIpcDecoderInit(decoder), error);
  if (!DecodeMessage(decoder, wire_schema, NANOARROW_IPC_MESSAGE_TYPE_SCHEMA, error).empty()) {
    throw ApiError::Invalid("Arrow writer_schema must not have a message body");
  }
  if (decoder->feature_flags != 0 || decoder->endianness != ArrowIpcSystemEndianness()) {
    throw ApiError::Unsupported("Arrow IPC schema features or byte order");
  }
  nanoarrow::UniqueSchema schema;
  Check(ArrowIpcDecoderDecodeSchema(decoder, schema.get(), &error), error);
  if (schema->n_children == 0) {
    throw ApiError::Unsupported("empty Arrow writer schemas");
  }
  fields_ = BindFields(*schema.get(), destination, 0);
  Check(ArrowIpcDecoderSetSchema(decoder, schema.get(), &error), error);
}

json StorageArrowDecoder::Decode(std::string_view wire_batch) {
  ArrowError error{};
  auto* decoder = decoder_.get();
  const auto body =
      DecodeMessage(decoder, wire_batch, NANOARROW_IPC_MESSAGE_TYPE_RECORD_BATCH, error);
  if (decoder->codec != NANOARROW_IPC_COMPRESSION_TYPE_NONE) {
    throw ApiError::Unsupported("compressed Arrow writer batches");
  }
  ArrowArrayView* batch = nullptr;
  Check(ArrowIpcDecoderDecodeArrayView(decoder, Buffer(body), -1, &batch, &error), error);
  const auto body_size = static_cast<int64_t>(body.size());
  auto remaining = body_size;
  CheckLayout(*batch, body_size, remaining);
  Check(ArrowArrayViewValidate(batch, NANOARROW_VALIDATION_LEVEL_FULL, &error), error);
  CheckNullCounts(*batch);
  auto rows = json::array();
  for (int64_t row = 0; row < batch->length; ++row) {
    rows.push_back(DecodeStruct(batch, row, fields_));
  }
  return rows;
}

}  // namespace bigquery_emulator_duckdb
