#pragma once

#include <stdexcept>

namespace bigquery_emulator_duckdb {

// An error from DuckDB, such as a failed connection, statement or function registration.
class BackendError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

}  // namespace bigquery_emulator_duckdb
