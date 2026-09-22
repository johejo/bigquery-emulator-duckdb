#pragma once

#include <string>

#include "src/frontend.h"

namespace bigquery_emulator_duckdb {

std::string TranslateToDuckDbSql(const FrontendResult& frontend_result);

}  // namespace bigquery_emulator_duckdb
