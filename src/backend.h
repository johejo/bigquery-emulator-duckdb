#pragma once

#include <string>

namespace bigquery_emulator_duckdb {

std::string ExecuteScalarString(const std::string& sql);

}  // namespace bigquery_emulator_duckdb
