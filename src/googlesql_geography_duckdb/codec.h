#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "src/googlesql_geography/value.h"

namespace bigquery_emulator_duckdb::geography_duckdb {

// The DuckDB type of stored GEOGRAPHY columns: a BLOB under an alias of its own, which casts to
// and from the plain BLOB that query values are.
inline constexpr char kGeographyStorageType[] = "bq_geography";

// DuckDB query values and stored columns hold a GEOGRAPHY as a BLOB of its points, linestrings
// and polygon rings, each vertex as the exact coordinates of its S2Point, followed for a nonempty
// geography by the encoded S2ShapeIndex of the shapes AddShapes adds for it.
std::string EncodeGeography(const googlesql_geography::Geography& geography);

// The geography of `encoded`, without its index.
absl::StatusOr<googlesql_geography::Geography> DecodeGeography(std::string_view encoded);

// The geography of `encoded` with the index it encodes, which reads `encoded` as it is used.
absl::StatusOr<googlesql_geography::IndexedGeography> DecodeIndexedGeography(std::string encoded);

// For debugging: `geography` as WKT of the XYZ coordinates of its S2Points, which round-trip, with
// polygon rings as S2 holds them, GEOMETRYCOLLECTION EMPTY and fullglobe. This is not external WKT.
std::string GeographyText(const googlesql_geography::Geography& geography);

}  // namespace bigquery_emulator_duckdb::geography_duckdb
