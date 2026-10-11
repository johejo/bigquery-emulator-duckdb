#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "google/protobuf/descriptor.pb.h"
#include "nlohmann/json.hpp"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

// Storage Read wire encoders. Cells use QueryResult's {v: ...}/{f: ...} representation.
std::string StorageReadSchema(const std::vector<FieldSchema>& schema, bool arrow);
std::string StorageReadRows(const std::vector<FieldSchema>& schema,
                            std::span<const nlohmann::json> rows, bool arrow);

// The self-contained proto2 writer descriptor of one AppendRows connection. Decodes to the
// named JSON values that WriteStorageRows validates against the destination table.
class StorageProtoDecoder {
 public:
  StorageProtoDecoder(const google::protobuf::DescriptorProto& descriptor,
                      const std::vector<FieldSchema>& schema);
  ~StorageProtoDecoder();
  StorageProtoDecoder(const StorageProtoDecoder&) = delete;
  StorageProtoDecoder& operator=(const StorageProtoDecoder&) = delete;
  [[nodiscard]] nlohmann::json Decode(const std::string& bytes) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bigquery_emulator_duckdb
