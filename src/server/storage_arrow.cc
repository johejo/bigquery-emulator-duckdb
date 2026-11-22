#include "src/server/storage_arrow.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/common/utf_util.h"
#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow.hpp"
#include "nanoarrow/nanoarrow_ipc.h"
#include "nanoarrow/nanoarrow_ipc.hpp"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/field_schema.h"
#include "src/server/storage_codec.h"

namespace bigquery_emulator_duckdb {
namespace {

void Check(int code, const ArrowError& error) {
  if (code != NANOARROW_OK) {
    throw ApiError::Invalid("Invalid Arrow IPC message: " + std::string(error.message));
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

}  // namespace

class StorageArrowDecoder::Impl {
 public:
  nanoarrow::ipc::UniqueDecoder decoder;
  std::vector<std::string> names;
};

StorageArrowDecoder::StorageArrowDecoder(std::string_view wire_schema,
                                         const std::vector<FieldSchema>& destination)
    : impl_(std::make_unique<Impl>()) {
  ValidateStorageWriteSchema(destination);
  ArrowError error{};
  auto& state = *impl_;
  auto* decoder = state.decoder.get();
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
  std::set<std::string_view> seen;
  for (int64_t i = 0; i < schema->n_children; ++i) {
    const auto& field = *schema->children[i];
    const auto found =
        std::ranges::find(destination, std::string_view(field.name), &FieldSchema::name);
    if (found == destination.end() || !seen.insert(field.name).second) {
      throw ApiError::Invalid("Arrow writer schema has an unknown or duplicate field: " +
                              std::string(field.name));
    }
    if (field.n_children != 0 || field.dictionary != nullptr || field.metadata != nullptr ||
        found->mode == FieldMode::kRepeated ||
        (found->type != FieldType::kInteger && found->type != FieldType::kString)) {
      throw ApiError::Unsupported("Arrow writer field type or metadata: " + found->name);
    }
    if (std::string_view(field.format) != (found->type == FieldType::kInteger ? "l" : "u")) {
      throw ApiError::Invalid("Arrow writer schema type does not match field: " + found->name);
    }
    state.names.push_back(found->name);
  }
  Check(ArrowIpcDecoderSetSchema(decoder, schema.get(), &error), error);
}

StorageArrowDecoder::~StorageArrowDecoder() = default;

nlohmann::json StorageArrowDecoder::Decode(std::string_view wire_batch) {
  ArrowError error{};
  auto& state = *impl_;
  auto* decoder = state.decoder.get();
  const auto body =
      DecodeMessage(decoder, wire_batch, NANOARROW_IPC_MESSAGE_TYPE_RECORD_BATCH, error);
  if (decoder->codec != NANOARROW_IPC_COMPRESSION_TYPE_NONE) {
    throw ApiError::Unsupported("compressed Arrow writer batches");
  }
  ArrowArrayView* batch = nullptr;
  Check(ArrowIpcDecoderDecodeArrayView(decoder, Buffer(body), -1, &batch, &error), error);
  // A nonempty schema has at least four bytes per row (a string offset or an int64).
  // Bound all lengths before full validation uses them in buffer size calculations.
  if (batch->length < 0 || std::cmp_greater(batch->length, body.size() / 4)) {
    throw ApiError::Invalid("Invalid Arrow row count");
  }
  auto remaining = static_cast<int64_t>(body.size());
  for (int64_t i = 0; i < batch->n_children; ++i) {
    const auto& col = *batch->children[i];
    for (const auto& buffer : col.buffer_views) {
      if (buffer.size_bytes > remaining) {
        throw ApiError::Invalid("Arrow column buffers exceed the batch body");
      }
      remaining -= buffer.size_bytes;
    }
    const auto alignment =
        col.storage_type == NANOARROW_TYPE_INT64 ? alignof(int64_t) : alignof(int32_t);
    const auto address = reinterpret_cast<uintptr_t>(
        col.buffer_views[1].data.data);  // NOLINT(cppcoreguidelines-pro-type-union-access)
    if (col.length != batch->length || col.null_count < 0 || col.null_count > col.length ||
        address % alignment != 0 ||
        (col.storage_type == NANOARROW_TYPE_STRING && col.buffer_views[1].size_bytes > 0 &&
         std::cmp_less(col.buffer_views[1].size_bytes, sizeof(int32_t)))) {
      throw ApiError::Invalid("Invalid Arrow column length or buffer alignment");
    }
  }
  Check(ArrowArrayViewValidate(batch, NANOARROW_VALIDATION_LEVEL_FULL, &error), error);
  for (int64_t i = 0; i < batch->n_children; ++i) {
    const auto* col = batch->children[i];
    if (col->null_count != ArrowArrayViewComputeNullCount(col)) {
      throw ApiError::Invalid("Arrow null count does not match the validity bitmap");
    }
  }
  auto rows = nlohmann::json::array();
  for (int64_t row = 0; row < batch->length; ++row) {
    auto values = nlohmann::json::object();
    for (int64_t i = 0; i < batch->n_children; ++i) {
      const auto& name = state.names.at(static_cast<size_t>(i));
      const auto* col = batch->children[i];
      if (ArrowArrayViewIsNull(col, row) != 0) {
        values[name] = nullptr;
      } else if (col->storage_type == NANOARROW_TYPE_INT64) {
        values[name] = std::to_string(ArrowArrayViewGetIntUnsafe(col, row));
      } else if (col->buffer_views[2].size_bytes == 0) {
        values[name] = "";
      } else {
        const auto text = ArrowArrayViewGetStringUnsafe(col, row);
        const std::string_view value(text.data, static_cast<size_t>(text.size_bytes));
        if (!googlesql::IsWellFormedUTF8(value)) {
          throw ApiError::Invalid("Invalid UTF-8 in Arrow writer field: " + name);
        }
        values[name] = value;
      }
    }
    rows.push_back(std::move(values));
  }
  return rows;
}

}  // namespace bigquery_emulator_duckdb
