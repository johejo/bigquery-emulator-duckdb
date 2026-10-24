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
using backend_functions::kBlob;
using backend_functions::kBoolean;
using backend_functions::kDouble;
using backend_functions::kVarchar;
using backend_functions::Register;
using googlesql_s2geometry::BoundingBox;
using googlesql_s2geometry::Geography;
using googlesql_s2geometry::IndexedGeography;
using googlesql_s2geometry::Options;

namespace {

// A GEOGRAPHY query value; see codec.h.
constexpr duckdb_type kGeography = kBlob;

absl::StatusOr<std::vector<Geography>> Decode(const std::vector<std::string>& values) {
  std::vector<Geography> result;
  result.reserve(values.size());
  for (const std::string& value : values) {
    GOOGLESQL_ASSIGN_OR_RETURN(Geography geography, DecodeGeography(value));
    result.push_back(std::move(geography));
  }
  return result;
}

template <typename T>
absl::StatusOr<std::remove_cvref_t<T>> ReadArgument(const Arguments& args, idx_t column) {
  using Value = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<Value, Geography>) {
    return DecodeGeography(args.String(column));
  } else if constexpr (std::is_same_v<Value, IndexedGeography>) {
    return DecodeIndexedGeography(args.String(column));
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

std::string Encode(const Geography& geography) { return EncodeGeography(geography); }

std::vector<std::string> Encode(const std::vector<Geography>& geographies) {
  std::vector<std::string> result;
  result.reserve(geographies.size());
  std::ranges::transform(geographies, std::back_inserter(result), EncodeGeography);
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
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, DecodeGeography(args.String(0)));
  return kFunction(geography);
}

template <S2BooleanOperation::OpType kOperation>
absl::StatusOr<std::string> SetOperation(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, DecodeGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, DecodeGeography(args.String(1)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography result,
                             googlesql_s2geometry::SetOperation(a, b, kOperation));
  return EncodeGeography(result);
}

absl::StatusOr<std::string> GeogFromText(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_s2geometry::ReadWkt(args.String(0)));
  return EncodeGeography(geography);
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
  return EncodeGeography(geography);
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
            GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, DecodeGeography(args.String(0)));
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
  const LogicalType geography(duckdb_create_logical_type(kGeography));
  const LogicalType dbl(duckdb_create_logical_type(kDouble));
  std::array<duckdb_logical_type, 4> fields = {dbl.get(), dbl.get(), dbl.get(), dbl.get()};
  std::array<const char*, 4> names = {"xmin", "ymin", "xmax", "ymax"};
  const LogicalType box(duckdb_create_struct_type(fields.data(), names.data(), fields.size()));
  const Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), "bq_st_boundingbox");
  duckdb_scalar_function_add_parameter(scalar.get(), geography.get());
  duckdb_scalar_function_set_return_type(scalar.get(), box.get());
  duckdb_scalar_function_set_function(scalar.get(), Bounds);
  duckdb_scalar_function_set_extra_info(scalar.get(), new Options(options),
                                        [](void* data) { delete static_cast<Options*>(data); });
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_st_boundingbox");
  }
}

// Copies each value between a stored GEOGRAPHY column and a query value, which hold the same
// encoding.
bool CopyGeographies(duckdb_function_info /*info*/, idx_t count, duckdb_vector input,
                     duckdb_vector output) {
  uint64_t* validity = duckdb_vector_get_validity(input);
  duckdb_vector_ensure_validity_writable(output);
  for (idx_t row = 0; row < count; ++row) {
    if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
      duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
      continue;
    }
    backend_functions::SetResult(output, row, VectorString(input, row));
  }
  return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a cast's source and target types.
void RegisterCopy(duckdb_connection connection, duckdb_logical_type source,
                  duckdb_logical_type target, int64_t implicit_cost) {
  const Handle<duckdb_cast_function, duckdb_destroy_cast_function> cast(
      duckdb_create_cast_function());
  duckdb_cast_function_set_source_type(cast.get(), source);
  duckdb_cast_function_set_target_type(cast.get(), target);
  duckdb_cast_function_set_function(cast.get(), CopyGeographies);
  if (implicit_cost >= 0) {
    duckdb_cast_function_set_implicit_cast_cost(cast.get(), implicit_cost);
  }
  if (duckdb_register_cast_function(connection, cast.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register a cast for stored GEOGRAPHY");
  }
}

// Stored columns read as query values wherever DuckDB expects one, and take them when written.
void RegisterGeographyStorage(duckdb_connection connection) {
  const LogicalType value(duckdb_create_logical_type(kGeography));
  const LogicalType stored(duckdb_create_logical_type(kGeography));
  duckdb_logical_type_set_alias(stored.get(), kGeographyStorageType);
  if (duckdb_register_logical_type(connection, stored.get(), nullptr) == DuckDBError) {
    throw BackendError("DuckDB failed to register GEOGRAPHY storage type");
  }
  RegisterCopy(connection, value.get(), stored.get(), -1);
  RegisterCopy(connection, stored.get(), value.get(), 0);
}

}  // namespace

void RegisterGeographyFunctions(duckdb_connection connection,
                                const googlesql_s2geometry::Options& options) {
  RegisterGeographyStorage(connection);
  // For debugging DuckDB SQL; GoogleSQL's catalog has no such function.
  Register(connection, "bq_geography_text", {kGeography}, kVarchar,
           EachRow<OfGeography<GeographyText>>);
  Register(connection, "bq_st_geogpoint", {kDouble, kDouble}, kGeography,
           EachRow<Call<googlesql_s2geometry::GeogPoint>>);
  Register(connection, "bq_st_geogfromtext", {kVarchar}, kGeography, EachRow<GeogFromText>);
  Register(connection, "bq_geography_input", {kVarchar}, kGeography, EachRow<GeographyInput>);
  Register(connection, "bq_st_geogpointfromgeohash", {kVarchar}, kGeography,
           EachRow<Call<googlesql_s2geometry::PointFromGeohash>>);
  Register(connection, "bq_st_astext", {kGeography}, kVarchar,
           EachRow<OfGeography<[](const Geography& geography) {
             return googlesql_s2geometry::Wkt(geography);
           }>>);
  Register(connection, "bq_st_asgeojson", {kGeography}, kVarchar,
           EachRow<Call<googlesql_s2geometry::AsGeoJson>>);
  Register(connection, "bq_st_angle", {kGeography, kGeography, kGeography}, kDouble,
           EachRow<Call<googlesql_s2geometry::Angle>>);
  Register(connection, "bq_st_centroid", {kGeography}, kGeography,
           EachRow<Call<googlesql_s2geometry::Centroid>>);
  Register(connection, "bq_st_convexhull", {kGeography}, kGeography,
           EachRow<Call<googlesql_s2geometry::ConvexHull>>);
  Register(connection, "bq_st_lineinterpolatepoint", {kGeography, kDouble}, kGeography,
           EachRow<Call<googlesql_s2geometry::LineInterpolatePoint>>);
  Register(connection, "bq_st_linelocatepoint", {kGeography, kGeography}, kDouble,
           EachRow<Call<googlesql_s2geometry::LineLocatePoint>>);
  Register(connection, "bq_st_linesubstring", {kGeography, kDouble, kDouble}, kGeography,
           EachRow<Call<googlesql_s2geometry::LineSubstring>>);
  Register(connection, "bq_st_azimuth", {kGeography, kGeography}, kDouble,
           EachRow<Call<googlesql_s2geometry::Azimuth>>);
  Register(connection, "bq_st_distance", {kGeography, kGeography, kBoolean, kVarchar}, kDouble,
           EachRow<Call<googlesql_s2geometry::Distance>>);
  Register(connection, "bq_st_hausdorffdistance", {kGeography, kGeography, kBoolean}, kDouble,
           EachRow<Call<googlesql_s2geometry::HausdorffDistance>>);
  Register(connection, "bq_st_hausdorffdwithin", {kGeography, kGeography, kBoolean, kDouble},
           kBoolean, EachRow<Call<googlesql_s2geometry::HausdorffDWithin>>);
  Register(connection, "bq_st_closestpoint", {kGeography, kGeography, kBoolean, kVarchar},
           kGeography, EachRow<Call<googlesql_s2geometry::ClosestPoint>>);
  Register(connection, "bq_st_intersectsbox", {kGeography, kDouble, kDouble, kDouble, kDouble},
           kBoolean, EachRow<Call<googlesql_s2geometry::IntersectsBox>>);
  Register(connection, "bq_st_dwithin", {kGeography, kGeography, kDouble, kBoolean, kVarchar},
           kBoolean, EachRow<Call<googlesql_s2geometry::DWithin>>);
  Register(connection, "bq_st_equals", {kGeography, kGeography}, kBoolean,
           EachRow<Call<googlesql_s2geometry::Equals>>);
  Register(connection, "bq_st_intersects", {kGeography, kGeography}, kBoolean,
           EachRow<Call<googlesql_s2geometry::Intersects>>);
  Register(connection, "bq_st_intersection", {kGeography, kGeography}, kGeography,
           EachRow<SetOperation<S2BooleanOperation::OpType::INTERSECTION>>);
  Register(connection, "bq_st_difference", {kGeography, kGeography}, kGeography,
           EachRow<SetOperation<S2BooleanOperation::OpType::DIFFERENCE>>);
  Register(connection, "bq_st_boundary", {kGeography}, kGeography,
           EachRow<Call<googlesql_s2geometry::Boundary>>);
  RegisterBounds(connection, options);
  Register(connection, "bq_st_touches", {kGeography, kGeography}, kBoolean,
           EachRow<Call<googlesql_s2geometry::Touches>>);
  Register(connection, "bq_st_covers", {kGeography, kGeography}, kBoolean,
           EachRow<Call<googlesql_s2geometry::Covers>>);
  Register(connection, "bq_st_contains", {kGeography, kGeography}, kBoolean,
           EachRow<Call<googlesql_s2geometry::Contains>>);
  Register(connection, "bq_st_x", {kGeography}, kDouble, EachRow<Call<googlesql_s2geometry::X>>);
  Register(connection, "bq_st_y", {kGeography}, kDouble, EachRow<Call<googlesql_s2geometry::Y>>);
  Register(connection, "bq_st_geometrytype", {kGeography}, kVarchar,
           EachRow<OfGeography<googlesql_s2geometry::GeometryType>>);
  Register(connection, "bq_st_dimension", {kGeography}, kBigint,
           EachRow<OfGeography<googlesql_s2geometry::Dimension>>);
  Register(connection, "bq_st_numpoints", {kGeography}, kBigint,
           EachRow<OfGeography<googlesql_s2geometry::NumPoints>>);
  Register(connection, "bq_st_isempty", {kGeography}, kBoolean,
           EachRow<OfGeography<[](const Geography& geography) { return geography.Empty(); }>>);
  Register(connection, "bq_st_iscollection", {kGeography}, kBoolean,
           EachRow<OfGeography<[](const Geography& geography) { return geography.Count() > 1; }>>);
  Register(connection, "bq_st_numgeometries", {kGeography}, kBigint,
           EachRow<OfGeography<[](const Geography& geography) {
             return static_cast<int64_t>(geography.Count());
           }>>);
  Register(connection, "bq_st_isclosed", {kGeography}, kBoolean,
           EachRow<OfGeography<googlesql_s2geometry::IsClosed>>);
  Register(connection, "bq_st_isring", {kGeography}, kBoolean,
           EachRow<OfGeography<googlesql_s2geometry::IsRing>>);
  Register(connection, "bq_st_length", {kGeography, kBoolean, kVarchar}, kDouble,
           EachRow<Call<googlesql_s2geometry::Length>>);
  Register(connection, "bq_st_measure", {kGeography, kBoolean, kVarchar}, kDouble,
           EachRow<Call<googlesql_s2geometry::Measure>>);
  Register(connection, "bq_st_exteriorring", {kGeography}, kGeography,
           EachRow<Call<googlesql_s2geometry::ExteriorRing>>);
  Register(connection, "bq_st_makepolygon", {kGeography}, kGeography,
           EachRow<[](const Arguments& args) { return MakePolygon({args.String(0)}); }>);
  Register(connection, "bq_st_pointn", {kGeography, kBigint}, kGeography,
           EachRow<Call<googlesql_s2geometry::PointN>>);
  Register(
      connection, "bq_st_makeline", {kGeography, kGeography}, kGeography,
      EachRow<[](const Arguments& args) { return MakeLine({args.String(0), args.String(1)}); }>);
  Register(connection, "bq_st_union", {kGeography, kGeography}, kGeography,
           EachRow<[](const Arguments& args) {
             return UnionGeographies({args.String(0), args.String(1)});
           }>);
  {
    const LogicalType geography(duckdb_create_logical_type(kGeography));
    const LogicalType list(duckdb_create_list_type(geography.get()));
    Register(connection, "bq_st_union_array", {list.get()}, geography.get(),
             EachRow<[](const Arguments& args) { return UnionGeographies(args.Strings(0)); }>);
    Register(connection, "bq_st_makeline_array", {list.get()}, geography.get(),
             EachRow<[](const Arguments& args) { return MakeLine(args.Strings(0)); }>);
    Register(connection, "bq_st_makepolygon_holes",
             std::vector<duckdb_logical_type>{geography.get(), list.get()}, geography.get(),
             EachRow<[](const Arguments& args) {
               std::vector<std::string> rings = args.Strings(1);
               rings.insert(rings.begin(), args.String(0));
               return MakePolygon(rings);
             }>);
    const LogicalType bigint(duckdb_create_logical_type(kBigint));
    Register(connection, "bq_st_dump",
             std::vector<duckdb_logical_type>{geography.get(), bigint.get()}, list.get(),
             EachRow<Call<googlesql_s2geometry::Dump>>);
    Register(connection, "bq_st_interiorrings", {geography.get()}, list.get(),
             EachRow<Call<googlesql_s2geometry::InteriorRings>>);
  }
  Register(connection, "bq_st_geohash", {kGeography, kBigint}, kVarchar,
           EachRow<Call<googlesql_s2geometry::Geohash>>);
  Register(connection, "bq_s2_cellidfrompoint", {kGeography, kBigint}, kBigint,
           EachRow<Call<googlesql_s2geometry::CellIdFromPoint>>);
}

}  // namespace bigquery_emulator_duckdb::geography_duckdb
