#pragma once

#include <string_view>

#include "absl/status/statusor.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {

// The geography of hex-encoded WKB, whose odd first digit BigQuery reads as a byte of its own.
// `failed`, `oriented` and `planar` are ReadWkb's.
absl::StatusOr<Geography> ReadHexWkb(std::string_view text, std::string_view failed, bool oriented,
                                     bool planar = false);

// The geography of `text` as ST_GEOGFROM reads a STRING, and loads and writes with `oriented` WKT
// and WKB: GeoJSON where it starts with '{', hex-encoded WKB where it has only hex digits, enough
// for a WKB header, and starts with a byte order, and WKT otherwise. `failed` prefixes WKT and WKB
// errors.
absl::StatusOr<Geography> ReadGeography(std::string_view text, bool oriented,
                                        std::string_view failed);

}  // namespace googlesql_geography
