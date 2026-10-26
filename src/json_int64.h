#ifndef BIGQUERY_EMULATOR_DUCKDB_SRC_JSON_INT64_H_
#define BIGQUERY_EMULATOR_DUCKDB_SRC_JSON_INT64_H_

#include <cstdint>
#include <optional>

#include "nlohmann/json_fwd.hpp"

namespace bigquery_emulator_duckdb {

// A JSON integer or decimal string within the int64 range; otherwise nullopt.
std::optional<int64_t> ParseJsonInt64(const nlohmann::json& value);

}  // namespace bigquery_emulator_duckdb

#endif  // BIGQUERY_EMULATOR_DUCKDB_SRC_JSON_INT64_H_
