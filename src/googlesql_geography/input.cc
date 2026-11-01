#include "src/googlesql_geography/input.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "src/googlesql_geography/geojson.h"
#include "src/googlesql_geography/value.h"
#include "src/googlesql_geography/wkb.h"
#include "src/googlesql_geography/wkt.h"

namespace googlesql_geography {
namespace {

// The fewest hex digits ST_GEOGFROM reads as WKB, those of its byte order and type.
constexpr size_t kWkbHeaderDigits = 10;

}  // namespace

absl::StatusOr<Geography> ReadHexWkb(std::string_view text, std::string_view failed, bool oriented,
                                     bool planar, bool make_valid) {
  const auto* const invalid =
      std::ranges::find_if(text, [](unsigned char c) { return std::isxdigit(c) == 0; });
  std::string bytes;
  if (invalid != text.end() ||
      !absl::HexStringToBytes((text.size() % 2 == 0 ? "" : "0") + std::string(text), &bytes)) {
    return absl::OutOfRangeError(
        std::string(failed) +
        "Failed to decode invalid hexadecimal string due to character at offset " +
        std::to_string(invalid - text.begin()) + ": " + std::string(text));
  }
  return ReadWkb(bytes, failed, oriented, planar, make_valid);
}

absl::StatusOr<Geography> ReadGeography(std::string_view text, bool oriented,
                                        std::string_view failed) {
  const size_t start = text.find_first_not_of(" \t\r\n");
  if (start != std::string_view::npos && text.at(start) == '{') {
    return ReadGeoJson(text);
  }
  if (text.size() >= kWkbHeaderDigits && (text.starts_with("00") || text.starts_with("01")) &&
      std::ranges::all_of(text, [](unsigned char c) { return std::isxdigit(c) != 0; })) {
    return ReadHexWkb(text, failed, oriented);
  }
  return ReadWkt(text, oriented, failed);
}

}  // namespace googlesql_geography
