#pragma once

#include <cstdint>
#include <string>
#include <string_view>
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
  [[nodiscard]] nlohmann::json Decode(std::string_view wire_batch);

 private:
  // Writer-order bindings; only the destination type, name, and Arrow decimal parameters
  // are needed during batch decoding. Nested fields have their own independent order.
  struct Field {
    std::string name;
    FieldType type;
    bool repeated;
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
};

}  // namespace bigquery_emulator_duckdb
