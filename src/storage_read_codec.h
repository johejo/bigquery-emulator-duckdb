#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/field_schema.h"

struct ArrowArray;
struct ArrowSchema;

namespace bigquery_emulator_duckdb {

// Storage Read wire encoders.

// The Avro schema JSON, or the encapsulated Arrow IPC schema message, of `schema`.
std::string StorageReadSchema(const std::vector<FieldSchema>& schema, bool arrow);

// Initializes `out` as the record batch type of `schema`: a non-nullable struct of its fields,
// each of the Arrow type Storage Read gives it. JSON and GEOGRAPHY use strings; Arrow output
// marks GEOGRAPHY and RANGE fields with metadata and rejects JSON.
void StorageReadArraySchema(ArrowSchema* out, const std::vector<FieldSchema>& schema, bool arrow);

// Avro binary rows from the same arrays used for Arrow output.
std::string EncodeAvroRows(const ArrowSchema& root, const ArrowArray& array,
                           const std::vector<FieldSchema>& schema);

enum class StorageReadCompression : uint8_t { kNone, kLz4Frame, kZstd };

// One encapsulated IPC record batch message of `array`, a struct array of the type `root`.
std::string EncodeArrowRecordBatch(
    const ArrowSchema& root, const ArrowArray& array,
    StorageReadCompression compression = StorageReadCompression::kNone);

// Raw LZ4 block for response_compression_codec; returns empty if compression is not smaller.
std::string CompressStorageReadResponse(const std::string& bytes);

}  // namespace bigquery_emulator_duckdb
