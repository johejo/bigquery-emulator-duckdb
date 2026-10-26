#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "google/protobuf/descriptor.pb.h"
#include "nlohmann/json.hpp"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

// A writer schema field whose protobuf type cannot hold its column's, which ends the AppendRows
// connection in BigQuery.
class StorageFieldMismatch : public std::invalid_argument {
 public:
  StorageFieldMismatch(std::string field, std::string proto_type, FieldType type)
      : std::invalid_argument("Writer schema type does not match field: " + field),
        field(std::move(field)),
        proto_type(std::move(proto_type)),
        type(type) {}
  std::string field;       // The protobuf field's full name, such as Row.g.
  std::string proto_type;  // Such as bytes.
  FieldType type;
};

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
