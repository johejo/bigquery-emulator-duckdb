#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string_view>

#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb::shell {

enum class OutputFormat : std::uint8_t { kPretty, kJson, kJsonl, kCsv };

std::optional<OutputFormat> ParseOutputFormat(std::string_view name);

// Writes the rows of a jobs.query response, whose values keep the API's encoding.
void WriteRows(const nlohmann::json& response, OutputFormat format, std::ostream& out);

}  // namespace bigquery_emulator_duckdb::shell
