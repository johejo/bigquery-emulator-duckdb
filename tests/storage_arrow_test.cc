#include "src/server/storage_arrow.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow.hpp"
#include "nanoarrow/nanoarrow_ipc.h"
#include "nanoarrow/nanoarrow_ipc.hpp"
#include "src/api_error.h"
#include "src/field_schema.h"
#include "zstd.h"

namespace bigquery_emulator_duckdb {
namespace {

// The uncompressed length each compressed buffer claims, larger than a batch may decompress to.
constexpr uint64_t kClaimedLength = uint64_t{1024} * 1024 * 1024;

// Compresses a buffer with ZSTD but records kClaimedLength as its uncompressed length.
int CompressOverstated(ArrowBufferView input, ArrowBuffer* output, ArrowError* /*error*/) {
  std::string compressed(ZSTD_compressBound(static_cast<size_t>(input.size_bytes)), '\0');
  const void* data = input.data.data;  // NOLINT(cppcoreguidelines-pro-type-union-access)
  const size_t size = ZSTD_compress(compressed.data(), compressed.size(), data,
                                    static_cast<size_t>(input.size_bytes), 1);
  if (ZSTD_isError(size) != 0) {
    return EIO;
  }
  uint64_t length = kClaimedLength;
  for (int i = 0; i < 8; ++i) {
    NANOARROW_RETURN_NOT_OK(ArrowBufferAppendUInt8(output, static_cast<uint8_t>(length & 255U)));
    length >>= 8U;
  }
  return ArrowBufferAppend(output, compressed.data(), static_cast<int64_t>(size));
}

std::string Message(ArrowIpcEncoder* encoder, const ArrowBuffer* body) {
  nanoarrow::UniqueBuffer header;
  EXPECT_EQ(ArrowIpcEncoderFinalizeBuffer(encoder, 1, header.get()), NANOARROW_OK);
  std::string message(reinterpret_cast<const char*>(header->data),
                      static_cast<size_t>(header->size_bytes));
  if (body != nullptr) {
    message.append(reinterpret_cast<const char*>(body->data),
                   static_cast<size_t>(body->size_bytes));
  }
  return message;
}

// A compressed batch whose buffers claim to decompress to more than the decoder allows fails
// before the decoder allocates what they claim.
TEST(StorageArrowDecoderTest, RejectsOverstatedDecompressedLength) {
  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "id"), NANOARROW_OK);
  nanoarrow::UniqueArray array;
  ASSERT_EQ(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayStartAppending(array.get()), NANOARROW_OK);
  for (int64_t id = 0; id < 4; ++id) {
    ASSERT_EQ(ArrowArrayAppendInt(array->children[0], id), NANOARROW_OK);
    ASSERT_EQ(ArrowArrayFinishElement(array.get()), NANOARROW_OK);
  }
  ASSERT_EQ(ArrowArrayFinishBuildingDefault(array.get(), nullptr), NANOARROW_OK);
  nanoarrow::UniqueArrayView view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(view.get(), schema.get(), nullptr), NANOARROW_OK);
  ASSERT_EQ(ArrowArrayViewSetArray(view.get(), array.get(), nullptr), NANOARROW_OK);

  nanoarrow::ipc::UniqueEncoder encoder;
  ASSERT_EQ(ArrowIpcEncoderInit(encoder.get()), NANOARROW_OK);
  ASSERT_EQ(ArrowIpcEncoderEncodeSchema(encoder.get(), schema.get(), nullptr), NANOARROW_OK);
  const std::string wire_schema = Message(encoder.get(), nullptr);
  nanoarrow::UniqueBuffer body;
  ASSERT_EQ(ArrowIpcEncoderEncodeCompressedRecordBatch(encoder.get(), view.get(), body.get(),
                                                       NANOARROW_IPC_COMPRESSION_TYPE_ZSTD,
                                                       CompressOverstated, nullptr),
            NANOARROW_OK);
  const std::string batch = Message(encoder.get(), body.get());

  StorageArrowDecoder decoder(wire_schema,
                              {FieldSchema{.name = "id", .type = FieldType::kInteger}});
  try {
    (void)decoder.Decode(batch, false, {});
    FAIL() << "decoded an overstated batch";
  } catch (const ApiError& error) {
    EXPECT_NE(std::string(error.what()).find("decompresses to more than"), std::string::npos)
        << error.what();
  }
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
