#pragma once

#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"
#include "src/field_schema.h"

struct ArrowArray;
struct ArrowSchema;

namespace bigquery_emulator_duckdb {

// Storage Read wire encoders. Cells use QueryResult's {v: ...}/{f: ...} representation.

// The Avro schema JSON, or the encapsulated Arrow IPC schema message, of `schema`.
std::string StorageReadSchema(const std::vector<FieldSchema>& schema, bool arrow);

// Initializes `out` as the record batch type of `schema`: a non-nullable struct of its fields,
// each of the Arrow type Storage Read gives it. For Avro, JSON and GEOGRAPHY use strings;
// Arrow output rejects them.
void StorageReadArraySchema(ArrowSchema* out, const std::vector<FieldSchema>& schema, bool arrow);

// Appends a cell's value to an array building the type StorageReadArraySchema gives `field`.
void AppendArrowCell(ArrowArray* array, const FieldSchema& field, const nlohmann::json& value);
// Avro binary rows from the same arrays used for Arrow output.
std::string EncodeAvroRows(const ArrowSchema& root, const ArrowArray& array,
                           const std::vector<FieldSchema>& schema);

// One encapsulated IPC record batch message of `array`, a struct array of the type `root`.
std::string EncodeArrowRecordBatch(const ArrowSchema& root, const ArrowArray& array);

}  // namespace bigquery_emulator_duckdb
