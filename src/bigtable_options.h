#pragma once

#include <string>
#include <string_view>

#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

struct BigtableSource {
  std::string project;
  std::string instance;
  std::string profile;
  std::string table;
};

BigtableOptions ParseBigtableOptions(std::string_view json);
BigtableSource ParseBigtableSource(const GcsExternalData& definition);
void ValidateBigtableExternal(const GcsExternalData& definition);

}  // namespace bigquery_emulator_duckdb
