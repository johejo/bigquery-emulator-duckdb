#pragma once

#include "duckdb.h"
namespace googlesql_s2geometry {
struct Options;
}

namespace bigquery_emulator_duckdb::geography_duckdb {

// The embedding application supplies compatibility choices; registration stores its own copy.
void RegisterGeographyFunctions(duckdb_connection connection,
                                const googlesql_s2geometry::Options& options);

}  // namespace bigquery_emulator_duckdb::geography_duckdb
