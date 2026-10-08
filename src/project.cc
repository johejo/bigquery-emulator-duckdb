#include "src/project.h"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace bigquery_emulator_duckdb {

Project ParseProject(const nlohmann::json& value) {
  if (!value.is_object()) throw std::invalid_argument("Project must be a JSON object");
  for (const auto& [key, field] : value.items()) {
    if (key != "projectId" && key != "numericId" && key != "friendlyName") {
      throw std::invalid_argument("Unknown project field: " + key);
    }
    if (!field.is_string()) throw std::invalid_argument("Project field must be a string: " + key);
  }
  if (!value.contains("projectId")) throw std::invalid_argument("projectId is required");
  Project project{.project_id = value.at("projectId").get<std::string>()};
  if (project.project_id.empty() || project.project_id.find('/') != std::string::npos) {
    throw std::invalid_argument("projectId must be nonempty and contain no slash");
  }
  if (value.contains("numericId")) {
    project.numeric_id = value.at("numericId").get<std::string>();
    const std::string& text = *project.numeric_id;
    uint64_t number = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (error != std::errc() || end != text.data() + text.size() || number == 0 ||
        text != std::to_string(number)) {
      throw std::invalid_argument("numericId must be a canonical positive uint64 string");
    }
  }
  if (value.contains("friendlyName")) {
    project.friendly_name = value.at("friendlyName").get<std::string>();
  }
  return project;
}

Project ReadProjectArgument(std::string_view argument) {
  if (!argument.starts_with('@')) return ParseProject(nlohmann::json::parse(argument));
  const std::string path(argument.substr(1));
  if (path.empty() || path == "-") throw std::invalid_argument("--project @ requires a file path");
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Cannot read project file: " + path);
  return ParseProject(nlohmann::json::parse(input));
}

nlohmann::json ProjectJson(const Project& project) {
  nlohmann::json value = {{"projectId", project.project_id}};
  if (project.numeric_id) value["numericId"] = *project.numeric_id;
  if (project.friendly_name) value["friendlyName"] = *project.friendly_name;
  return value;
}

}  // namespace bigquery_emulator_duckdb
