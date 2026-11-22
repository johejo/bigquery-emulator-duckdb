#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

// Arrow IPC writer schema for one AppendRows connection. Decode returns the same named JSON
// rows as the protobuf path, so both formats share destination validation and publishing.
class StorageArrowDecoder {
 public:
  StorageArrowDecoder(std::string_view wire_schema, const std::vector<FieldSchema>& destination);
  ~StorageArrowDecoder();
  StorageArrowDecoder(const StorageArrowDecoder&) = delete;
  StorageArrowDecoder& operator=(const StorageArrowDecoder&) = delete;

  [[nodiscard]] nlohmann::json Decode(std::string_view wire_batch);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bigquery_emulator_duckdb
