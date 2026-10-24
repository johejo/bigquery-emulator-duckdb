#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::text_analysis_duckdb {

void RegisterTextAnalysisFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::text_analysis_duckdb
