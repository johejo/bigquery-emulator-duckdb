#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {

// A column described with BigQuery's TableFieldSchema vocabulary.
struct FieldSchema {
  std::string name;
  std::string type;                 // INTEGER, FLOAT, STRING, BOOLEAN, TIMESTAMP, RECORD, ...
  std::string mode;                 // NULLABLE or REPEATED
  std::vector<FieldSchema> fields;  // Populated for RECORD.

  nlohmann::json ToJson() const;
};

}  // namespace bigquery_emulator_duckdb
