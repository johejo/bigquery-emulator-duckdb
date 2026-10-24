#include "src/googlesql_s2geometry_duckdb/geography.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "s2/s2boolean_operation.h"
#include "src/backend_error.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_s2geometry/geography.h"
#include "src/googlesql_s2geometry/options.h"
#include "src/googlesql_s2geometry/value.h"
#include "src/googlesql_s2geometry/wkt.h"
#include "src/googlesql_s2geometry_duckdb/codec.h"

namespace bigquery_emulator_duckdb::geography_duckdb {
using backend_functions::Arguments;
using backend_functions::EachRow;
using backend_functions::kBigint;
using backend_functions::kBoolean;
using backend_functions::kDouble;
using backend_functions::kVarchar;
using backend_functions::Register;
using googlesql_s2geometry::BoundingBox;
using googlesql_s2geometry::Geography;
using googlesql_s2geometry::Options;

namespace {

absl::StatusOr<std::vector<Geography>> Decode(const std::vector<std::string>& values) {
  std::vector<Geography> result;
  result.reserve(values.size());
  for (const std::string& value : values) {
    GOOGLESQL_ASSIGN_OR_RETURN(Geography geography, ParseGeography(value));
    result.push_back(std::move(geography));
  }
  return result;
}

template <typename T>
absl::StatusOr<std::remove_cvref_t<T>> ReadArgument(const Arguments& args, idx_t column) {
  using Value = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<Value, Geography>) {
    return ParseGeography(args.String(column));
  } else if constexpr (std::is_same_v<Value, double>) {
    return args.Double(column);
  } else if constexpr (std::is_same_v<Value, int64_t>) {
    return args.Int(column);
  } else if constexpr (std::is_same_v<Value, bool>) {
    return args.Bool(column);
  } else {
    static_assert(std::is_same_v<Value, std::string>, "Unsupported geography argument type");
    return args.String(column);
  }
}

template <typename T>
T Encode(T value) {
  return value;
}

std::string Encode(const Geography& geography) { return GeographyText(geography); }

std::vector<std::string> Encode(const std::vector<Geography>& geographies) {
  std::vector<std::string> result;
  result.reserve(geographies.size());
  std::ranges::transform(geographies, std::back_inserter(result), GeographyText);
  return result;
}

template <typename T>
auto Encode(const std::optional<T>& value) -> std::optional<decltype(Encode(*value))> {
  if (!value) {
    return std::nullopt;
  }
  return Encode(*value);
}

// Decode arguments in order and propagate function errors unchanged. SQL NULL inputs
// and encoded results are handled by the common EachRow callback.
template <typename Result, typename... Parameters, size_t... Index>
auto Invoke(const Arguments& args, absl::StatusOr<Result> (*function)(Parameters...),
            std::index_sequence<Index...> /*indices*/)
    -> absl::StatusOr<decltype(Encode(std::declval<Result>()))> {
  std::tuple inputs{ReadArgument<Parameters>(args, Index)...};
  absl::Status status;
  std::apply([&](const auto&... input) { ((status = status.ok() ? input.status() : status), ...); },
             inputs);
  if (!status.ok()) {
    return status;
  }
  auto result = std::apply([&](auto&... input) { return function(*input...); }, inputs);
  if (!result.ok()) {
    return result.status();
  }
  return Encode(*result);
}

template <typename Result, typename... Parameters>
auto Invoke(const Arguments& args, absl::StatusOr<Result> (*function)(Parameters...)) {
  return Invoke(args, function, std::index_sequence_for<Parameters...>{});
}

template <auto kFunction>
auto Call(const Arguments& args) {
  return Invoke(args, kFunction);
}

// Functions with plain results, rather than StatusOr, still decode a GEOGRAPHY input.
template <auto kFunction>
absl::StatusOr<decltype(kFunction(Geography{}))> OfGeography(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  return kFunction(geography);
}

template <S2BooleanOperation::OpType kOperation>
absl::StatusOr<std::string> SetOperation(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, ParseGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, ParseGeography(args.String(1)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography result,
                             googlesql_s2geometry::SetOperation(a, b, kOperation));
  return GeographyText(result);
}

absl::StatusOr<std::string> GeogFromText(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_s2geometry::ReadWkt(args.String(0)));
  return GeographyText(geography);
}
// A GEOGRAPHY that a load or tabledata.insertAll writes, given as WKT whose polygons are oriented.
// BigQuery also takes GeoJSON and WKB, hex-encoded or, from Parquet, binary, which DuckDB casts to
// text that starts with the escape of its first byte; these are unsupported.
absl::StatusOr<std::string> GeographyInput(const Arguments& args) {
  const std::string text = args.String(0);
  const size_t start = text.find_first_not_of(" \t\r\n");
  if (start != std::string::npos && text.at(start) == '{') {
    return absl::UnimplementedError("The emulator does not support GeoJSON GEOGRAPHY values");
  }
  if (text.starts_with("\\x") || (!text.empty() && std::ranges::all_of(text, [](unsigned char c) {
        return std::isxdigit(c) != 0;
      }))) {
    return absl::UnimplementedError("The emulator does not support WKB GEOGRAPHY values");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_s2geometry::ReadWkt(text, /*oriented=*/true));
  return GeographyText(geography);
}

absl::StatusOr<std::optional<std::string>> UnionGeographies(
    const std::vector<std::string>& values) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Geography> geographies, Decode(values));
  auto result = googlesql_s2geometry::UnionGeographies(geographies);
  if (!result.ok()) {
    return result.status();
  }
  return Encode(*result);
}

absl::StatusOr<std::string> MakeLine(const std::vector<std::string>& values) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Geography> geographies, Decode(values));
  auto result = googlesql_s2geometry::MakeLine(geographies);
  if (!result.ok()) {
    return result.status();
  }
  return Encode(*result);
}

absl::StatusOr<std::string> MakePolygon(const std::vector<std::string>& values) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Geography> geographies, Decode(values));
  auto result = googlesql_s2geometry::MakePolygon(geographies);
  if (!result.ok()) {
    return result.status();
  }
  return Encode(*result);
}

struct BoxResult {
  BoundingBox box;
};

void SetResult(duckdb_vector output, idx_t row, const BoxResult& result) {
  const BoundingBox& box = result.box;
  const std::array<double, 4> values = {box.xmin, box.ymin, box.xmax, box.ymax};
  for (idx_t i = 0; i < values.size(); ++i) {
    backend_functions::SetResult(duckdb_struct_vector_get_child(output, i), row, values.at(i));
  }
}

void Bounds(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto& options = *static_cast<const Options*>(duckdb_scalar_function_get_extra_info(info));
  EachRow(info, input, output,
          [&](const Arguments& args) -> absl::StatusOr<std::optional<BoxResult>> {
            GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
            const auto box = googlesql_s2geometry::Bounds(geography, options);
            if (!box) {
              return std::nullopt;
            }
            return BoxResult{.box = *box};
          });
}

// The registered function owns a copy, so caller options may be temporary and different
// databases can use different policies without sharing mutable state.
void RegisterBounds(duckdb_connection connection, const Options& options) {
  const LogicalType varchar(duckdb_create_logical_type(kVarchar));
  const LogicalType dbl(duckdb_create_logical_type(kDouble));
  std::array<duckdb_logical_type, 4> fields = {dbl.get(), dbl.get(), dbl.get(), dbl.get()};
  std::array<const char*, 4> names = {"xmin", "ymin", "xmax", "ymax"};
  const LogicalType box(duckdb_create_struct_type(fields.data(), names.data(), fields.size()));
  const Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), "bq_st_boundingbox");
  duckdb_scalar_function_add_parameter(scalar.get(), varchar.get());
  duckdb_scalar_function_set_return_type(scalar.get(), box.get());
  duckdb_scalar_function_set_function(scalar.get(), Bounds);
  duckdb_scalar_function_set_extra_info(scalar.get(), new Options(options),
                                        [](void* data) { delete static_cast<Options*>(data); });
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_st_boundingbox");
  }
}

}  // namespace

void RegisterGeographyFunctions(duckdb_connection connection,
                                const googlesql_s2geometry::Options& options) {
  Register(connection, "bq_st_geogpoint", {kDouble, kDouble}, kVarchar,
           EachRow<Call<googlesql_s2geometry::GeogPoint>>);
  Register(connection, "bq_st_geogfromtext", {kVarchar}, kVarchar, EachRow<GeogFromText>);
  Register(connection, "bq_geography_input", {kVarchar}, kVarchar, EachRow<GeographyInput>);
  Register(connection, "bq_st_geogpointfromgeohash", {kVarchar}, kVarchar,
           EachRow<Call<googlesql_s2geometry::PointFromGeohash>>);
  Register(connection, "bq_st_astext", {kVarchar}, kVarchar,
           EachRow<OfGeography<[](const Geography& geography) {
             return googlesql_s2geometry::Wkt(geography);
           }>>);
  Register(connection, "bq_st_asgeojson", {kVarchar}, kVarchar,
           EachRow<Call<googlesql_s2geometry::AsGeoJson>>);
  Register(connection, "bq_st_angle", {kVarchar, kVarchar, kVarchar}, kDouble,
           EachRow<Call<googlesql_s2geometry::Angle>>);
  Register(connection, "bq_st_centroid", {kVarchar}, kVarchar,
           EachRow<Call<googlesql_s2geometry::Centroid>>);
  Register(connection, "bq_st_convexhull", {kVarchar}, kVarchar,
           EachRow<Call<googlesql_s2geometry::ConvexHull>>);
  Register(connection, "bq_st_lineinterpolatepoint", {kVarchar, kDouble}, kVarchar,
           EachRow<Call<googlesql_s2geometry::LineInterpolatePoint>>);
  Register(connection, "bq_st_linelocatepoint", {kVarchar, kVarchar}, kDouble,
           EachRow<Call<googlesql_s2geometry::LineLocatePoint>>);
  Register(connection, "bq_st_linesubstring", {kVarchar, kDouble, kDouble}, kVarchar,
           EachRow<Call<googlesql_s2geometry::LineSubstring>>);
  Register(connection, "bq_st_azimuth", {kVarchar, kVarchar}, kDouble,
           EachRow<Call<googlesql_s2geometry::Azimuth>>);
  Register(connection, "bq_st_distance", {kVarchar, kVarchar, kBoolean, kVarchar}, kDouble,
           EachRow<Call<googlesql_s2geometry::Distance>>);
  Register(connection, "bq_st_hausdorffdistance", {kVarchar, kVarchar, kBoolean}, kDouble,
           EachRow<Call<googlesql_s2geometry::HausdorffDistance>>);
  Register(connection, "bq_st_hausdorffdwithin", {kVarchar, kVarchar, kBoolean, kDouble}, kBoolean,
           EachRow<Call<googlesql_s2geometry::HausdorffDWithin>>);
  Register(connection, "bq_st_closestpoint", {kVarchar, kVarchar, kBoolean, kVarchar}, kVarchar,
           EachRow<Call<googlesql_s2geometry::ClosestPoint>>);
  Register(connection, "bq_st_intersectsbox", {kVarchar, kDouble, kDouble, kDouble, kDouble},
           kBoolean, EachRow<Call<googlesql_s2geometry::IntersectsBox>>);
  Register(connection, "bq_st_dwithin", {kVarchar, kVarchar, kDouble, kBoolean, kVarchar}, kBoolean,
           EachRow<Call<googlesql_s2geometry::DWithin>>);
  Register(connection, "bq_st_equals", {kVarchar, kVarchar}, kBoolean,
           EachRow<Call<googlesql_s2geometry::Equals>>);
  Register(connection, "bq_st_intersects", {kVarchar, kVarchar}, kBoolean,
           EachRow<Call<googlesql_s2geometry::Intersects>>);
  Register(connection, "bq_st_intersection", {kVarchar, kVarchar}, kVarchar,
           EachRow<SetOperation<S2BooleanOperation::OpType::INTERSECTION>>);
  Register(connection, "bq_st_difference", {kVarchar, kVarchar}, kVarchar,
           EachRow<SetOperation<S2BooleanOperation::OpType::DIFFERENCE>>);
  Register(connection, "bq_st_boundary", {kVarchar}, kVarchar,
           EachRow<Call<googlesql_s2geometry::Boundary>>);
  RegisterBounds(connection, options);
  Register(connection, "bq_st_touches", {kVarchar, kVarchar}, kBoolean,
           EachRow<Call<googlesql_s2geometry::Touches>>);
  Register(connection, "bq_st_covers", {kVarchar, kVarchar}, kBoolean,
           EachRow<Call<googlesql_s2geometry::Covers>>);
  Register(connection, "bq_st_contains", {kVarchar, kVarchar}, kBoolean,
           EachRow<Call<googlesql_s2geometry::Contains>>);
  Register(connection, "bq_st_x", {kVarchar}, kDouble, EachRow<Call<googlesql_s2geometry::X>>);
  Register(connection, "bq_st_y", {kVarchar}, kDouble, EachRow<Call<googlesql_s2geometry::Y>>);
  Register(connection, "bq_st_geometrytype", {kVarchar}, kVarchar,
           EachRow<OfGeography<googlesql_s2geometry::GeometryType>>);
  Register(connection, "bq_st_dimension", {kVarchar}, kBigint,
           EachRow<OfGeography<googlesql_s2geometry::Dimension>>);
  Register(connection, "bq_st_numpoints", {kVarchar}, kBigint,
           EachRow<OfGeography<googlesql_s2geometry::NumPoints>>);
  Register(connection, "bq_st_isempty", {kVarchar}, kBoolean,
           EachRow<OfGeography<[](const Geography& geography) { return geography.Empty(); }>>);
  Register(connection, "bq_st_iscollection", {kVarchar}, kBoolean,
           EachRow<OfGeography<[](const Geography& geography) { return geography.Count() > 1; }>>);
  Register(connection, "bq_st_numgeometries", {kVarchar}, kBigint,
           EachRow<OfGeography<[](const Geography& geography) {
             return static_cast<int64_t>(geography.Count());
           }>>);
  Register(connection, "bq_st_isclosed", {kVarchar}, kBoolean,
           EachRow<OfGeography<googlesql_s2geometry::IsClosed>>);
  Register(connection, "bq_st_isring", {kVarchar}, kBoolean,
           EachRow<OfGeography<googlesql_s2geometry::IsRing>>);
  Register(connection, "bq_st_length", {kVarchar, kBoolean, kVarchar}, kDouble,
           EachRow<Call<googlesql_s2geometry::Length>>);
  Register(connection, "bq_st_measure", {kVarchar, kBoolean, kVarchar}, kDouble,
           EachRow<Call<googlesql_s2geometry::Measure>>);
  Register(connection, "bq_st_exteriorring", {kVarchar}, kVarchar,
           EachRow<Call<googlesql_s2geometry::ExteriorRing>>);
  Register(connection, "bq_st_makepolygon", {kVarchar}, kVarchar,
           EachRow<[](const Arguments& args) { return MakePolygon({args.String(0)}); }>);
  Register(connection, "bq_st_pointn", {kVarchar, kBigint}, kVarchar,
           EachRow<Call<googlesql_s2geometry::PointN>>);
  Register(
      connection, "bq_st_makeline", {kVarchar, kVarchar}, kVarchar,
      EachRow<[](const Arguments& args) { return MakeLine({args.String(0), args.String(1)}); }>);
  Register(connection, "bq_st_union", {kVarchar, kVarchar}, kVarchar,
           EachRow<[](const Arguments& args) {
             return UnionGeographies({args.String(0), args.String(1)});
           }>);
  {
    const LogicalType varchar(duckdb_create_logical_type(kVarchar));
    const LogicalType list(duckdb_create_list_type(varchar.get()));
    Register(connection, "bq_st_union_array", {list.get()}, varchar.get(),
             EachRow<[](const Arguments& args) { return UnionGeographies(args.Strings(0)); }>);
    Register(connection, "bq_st_makeline_array", {list.get()}, varchar.get(),
             EachRow<[](const Arguments& args) { return MakeLine(args.Strings(0)); }>);
    Register(connection, "bq_st_makepolygon_holes",
             std::vector<duckdb_logical_type>{varchar.get(), list.get()}, varchar.get(),
             EachRow<[](const Arguments& args) {
               std::vector<std::string> rings = args.Strings(1);
               rings.insert(rings.begin(), args.String(0));
               return MakePolygon(rings);
             }>);
    const LogicalType bigint(duckdb_create_logical_type(kBigint));
    Register(connection, "bq_st_dump",
             std::vector<duckdb_logical_type>{varchar.get(), bigint.get()}, list.get(),
             EachRow<Call<googlesql_s2geometry::Dump>>);
    Register(connection, "bq_st_interiorrings", {varchar.get()}, list.get(),
             EachRow<Call<googlesql_s2geometry::InteriorRings>>);
  }
  Register(connection, "bq_st_geohash", {kVarchar, kBigint}, kVarchar,
           EachRow<Call<googlesql_s2geometry::Geohash>>);
  Register(connection, "bq_s2_cellidfrompoint", {kVarchar, kBigint}, kBigint,
           EachRow<Call<googlesql_s2geometry::CellIdFromPoint>>);
}

}  // namespace bigquery_emulator_duckdb::geography_duckdb
