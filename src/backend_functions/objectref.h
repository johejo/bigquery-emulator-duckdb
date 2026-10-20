#pragma once

#include <functional>
#include <string>

#include "duckdb.h"

namespace bigquery_emulator_duckdb::backend_functions {

// Registers the checks of OBJ.MAKE_REF's arguments, which find connections by
// PROJECT.LOCATION.CONNECTION with `has_connection`; none exist when it is empty.
void RegisterObjectRefFunctions(duckdb_connection connection,
                                const std::function<bool(const std::string&)>& has_connection);

}  // namespace bigquery_emulator_duckdb::backend_functions
