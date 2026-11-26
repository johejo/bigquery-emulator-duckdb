#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow_ipc.hpp"
#include "nlohmann/json.hpp"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

// Arrow IPC writer schema for one AppendRows connection. Decode returns the same named JSON
// rows as the protobuf path, so both formats share destination validation and publishing.
class StorageArrowDecoder {
 public:
  StorageArrowDecoder(std::string_view wire_schema, const std::vector<FieldSchema>& destination);
  // The decoder's decompressor refers to decompressed_bytes_.
  StorageArrowDecoder(const StorageArrowDecoder&) = delete;
  StorageArrowDecoder& operator=(const StorageArrowDecoder&) = delete;
  StorageArrowDecoder(StorageArrowDecoder&&) = delete;
  StorageArrowDecoder& operator=(StorageArrowDecoder&&) = delete;
  ~StorageArrowDecoder() = default;

  // A NULL in a top-level column with a default is a missing value: `missing` gives the
  // column's interpretation, true for DEFAULT_VALUE, and otherwise `default_missing` does. A
  // missing value to default is left out of its row, as a column the writer schema lacks is,
  // unless `missing` gives that column NULL_VALUE.
  [[nodiscard]] nlohmann::json Decode(std::string_view wire_batch, bool default_missing,
                                      const std::unordered_map<std::string, bool>& missing);

 private:
  // Writer-order bindings; only the destination type, name, default, and Arrow decimal
  // parameters are needed during batch decoding. Nested fields have their own independent order.
  struct Field {
    std::string name;
    FieldType type;
    bool repeated;
    bool has_default = false;
    int32_t precision = 0;
    int32_t scale = 0;
    std::vector<Field> children = {};
  };

  static Field Bind(const ArrowSchema& input, const FieldSchema& destination, int depth);
  static std::vector<Field> BindFields(const ArrowSchema& input,
                                       const std::vector<FieldSchema>& destination, int depth);
  static std::string DecodeDecimal(const ArrowArrayView* col, int64_t row, const Field& field);
  static nlohmann::json DecodeValue(const ArrowArrayView* col, int64_t row, const Field& field);
  static nlohmann::json DecodeStruct(const ArrowArrayView* col, int64_t row,
                                     const std::vector<Field>& fields);

  nanoarrow::ipc::UniqueDecoder decoder_;
  std::vector<Field> fields_;
  // Top-level destination columns with defaults that the writer schema lacks.
  std::vector<Field> absent_;
  // Bytes the current batch's buffers decompress to, which a batch may not exceed.
  int64_t decompressed_bytes_ = 0;
};

}  // namespace bigquery_emulator_duckdb
