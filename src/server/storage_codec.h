#pragma once

#include <memory>
#include <string>
#include <vector>

#include "google/protobuf/descriptor.pb.h"
#include "nlohmann/json.hpp"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

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
