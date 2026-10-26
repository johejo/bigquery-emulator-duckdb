#include "src/json_int64.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {

std::optional<int64_t> ParseJsonInt64(const nlohmann::json& value) {
  if (value.is_number_integer() &&
      (!value.is_number_unsigned() ||
       value.get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) {
    return value.get<int64_t>();
  }
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    int64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error == std::errc() && end == text.data() + text.size()) {
      return parsed;
    }
  }
  return std::nullopt;
}

}  // namespace bigquery_emulator_duckdb
