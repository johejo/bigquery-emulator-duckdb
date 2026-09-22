#pragma once

#include <string_view>

namespace bigquery_emulator_duckdb {

// The BigQuery v2 REST discovery document, embedded at build time from
// third_party/bigquery/discovery.json.
std::string_view DiscoveryDocument();

}  // namespace bigquery_emulator_duckdb
