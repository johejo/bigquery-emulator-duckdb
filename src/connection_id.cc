#include "src/connection_id.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"

namespace bigquery_emulator_duckdb {

std::optional<std::string> ConnectionKey(std::string_view connection_id,
                                         const std::string& default_project) {
  std::vector<std::string> parts;
  if (connection_id.starts_with("projects/")) {
    const std::vector<std::string> path = absl::StrSplit(connection_id, '/');
    if (path.size() != 6 || path.at(2) != "locations" || path.at(4) != "connections") {
      return std::nullopt;
    }
    parts = {path.at(1), path.at(3), path.at(5)};
  } else {
    // A domain-scoped project ID, such as example.com:project, has dots of its own.
    parts = absl::StrSplit(connection_id, '.');
    if (parts.size() > 3) {
      const std::vector<std::string> last(parts.end() - 2, parts.end());
      parts = {
          absl::StrJoin(std::vector<std::string>(parts.begin(), parts.end() - 2), "."),
          last.at(0),
          last.at(1),
      };
    } else if (parts.size() == 2 && !default_project.empty()) {
      parts.insert(parts.begin(), default_project);
    }
  }
  if (parts.size() != 3 || parts.at(0).empty() || parts.at(1).empty() || parts.at(2).empty() ||
      parts.at(2).find('/') != std::string::npos) {
    return std::nullopt;
  }
  return parts.at(0) + "." + absl::AsciiStrToLower(parts.at(1)) + "." + parts.at(2);
}

}  // namespace bigquery_emulator_duckdb
