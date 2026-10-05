#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {

struct Project {
  std::string project_id;
  std::optional<std::string> numeric_id = std::nullopt;
  std::optional<std::string> friendly_name = std::nullopt;
};

Project ParseProject(const nlohmann::json& value);
Project ReadProjectArgument(std::string_view argument);
nlohmann::json ProjectJson(const Project& project);

}  // namespace bigquery_emulator_duckdb
