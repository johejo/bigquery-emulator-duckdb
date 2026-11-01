#include "src/googlesql_geography_duckdb/geography.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/type.pb.h"
#include "s2/s2boolean_operation.h"
#include "src/backend_error.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/function_catalog/rules.h"
#include "src/function_catalog/static_definitions.h"
#include "src/googlesql_geography/geography.h"
#include "src/googlesql_geography/geojson.h"
#include "src/googlesql_geography/input.h"
#include "src/googlesql_geography/options.h"
#include "src/googlesql_geography/value.h"
#include "src/googlesql_geography/wkb.h"
#include "src/googlesql_geography/wkt.h"
#include "src/googlesql_geography_duckdb/codec.h"
#include "src/googlesql_geography_duckdb/functions.h"

namespace bigquery_emulator_duckdb::geography_duckdb {

using duckdb_functions::Arguments;
using duckdb_functions::EachRow;
using duckdb_functions::kBigint;
using duckdb_functions::kBlob;
using duckdb_functions::kBoolean;
using duckdb_functions::kDouble;
using duckdb_functions::kVarchar;
using duckdb_functions::Register;
using function_catalog::Define;
using function_catalog::DefinitionViews;
using googlesql_geography::BoundingBox;
using googlesql_geography::Geography;
using googlesql_geography::IndexedGeography;
using googlesql_geography::Options;

namespace {

// A GEOGRAPHY query value; see codec.h.
constexpr duckdb_type kGeography = kBlob;

// The functions over a list of values, which also finish aggregates.
constexpr char kUnionArray[] = "bq_st_union_array";
constexpr char kCentroidAggregate[] = "bq_st_centroid_agg";
constexpr char kExtent[] = "bq_st_extent";

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
                             googlesql_geography::SetOperation(a, b, kOperation));
  return EncodeGeography(result);
}

absl::StatusOr<std::string> GeogFromText(const Arguments& args) {
  if (args.Bool(1) && args.Bool(2)) {
    return absl::OutOfRangeError(
        "ST_GeogFromText failed: Options planar and oriented are not compatible.");
  }
  if (args.Bool(1) && args.Bool(3)) {
    return absl::OutOfRangeError(
        "ST_GeogFromText failed: Options make_valid and oriented are not compatible.");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      const Geography geography,
      googlesql_geography::ReadWkt(args.String(0), args.Bool(1),
                                   "ST_GeogFromText failed: ", args.Bool(2), args.Bool(3)));
  return EncodeGeography(geography);
}

absl::StatusOr<std::string> GeogFromGeoJson(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_geography::ReadGeoJson(args.String(0), args.Bool(1)));
  return EncodeGeography(geography);
}

absl::StatusOr<std::string> GeogFromWkb(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_geography::ReadWkb(args.String(0)));
  return EncodeGeography(geography);
}

absl::StatusOr<std::string> GeogFromHexWkb(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const Geography geography,
      googlesql_geography::ReadHexWkb(args.String(0), "ST_GeogFromWKBHex failed: ",
                                      /*oriented=*/false));
  return EncodeGeography(geography);
}

template <bool kHex>
absl::StatusOr<std::string> GeogFromOrientedWkb(const Arguments& args) {
  if (args.Bool(1) && args.Bool(2)) {
    return absl::OutOfRangeError(
        "ST_GeogFromWKB: Options planar and oriented are not compatible.; in planar WKB "
        "geography; error in ST_GeogFromWKB expression");
  }
  if (args.Bool(1) && args.Bool(3)) {
    return absl::OutOfRangeError(
        "ST_GeogFromWKB failed: Options make_valid and oriented are not compatible.");
  }
  const auto geography =
      kHex ? googlesql_geography::ReadHexWkb(args.String(0),
                                             "ST_GeogFromWKBHex failed: ", args.Bool(1),
                                             args.Bool(2), args.Bool(3))
           : googlesql_geography::ReadWkb(args.String(0), "ST_GeogFromWKB failed: ", args.Bool(1),
                                          args.Bool(2), args.Bool(3));
  if (!geography.ok()) {
    return geography.status();
  }
  return EncodeGeography(*geography);
}

// The encoded geography of `text` as googlesql_geography::ReadGeography reads it.
absl::StatusOr<std::string> ReadGeography(const std::string& text, bool oriented,
                                          std::string_view failed) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_geography::ReadGeography(text, oriented, failed));
  return EncodeGeography(geography);
}

constexpr std::string_view kGeogFromFailed = "ST_GeogFrom failed: ";

// ST_GEOGFROM fails on an empty STRING or BYTES before it reads either.
absl::Status Missing() {
  return absl::OutOfRangeError(std::string(kGeogFromFailed) + "Geography value is missing");
}

// ST_GEOGFROM of BYTES, which BigQuery reads as WKB.
absl::StatusOr<std::string> GeogFromBytes(const Arguments& args) {
  const std::string bytes = args.String(0);
  if (bytes.empty()) {
    return Missing();
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_geography::ReadWkb(bytes, kGeogFromFailed));
  return EncodeGeography(geography);
}

absl::StatusOr<std::string> GeogFrom(const Arguments& args) {
  const std::string text = args.String(0);
  if (text.empty()) {
    return Missing();
  }
  return ReadGeography(text, /*oriented=*/false, kGeogFromFailed);
}

// A GEOGRAPHY that a load or tabledata.insertAll writes, whose WKT and WKB polygons are oriented.
constexpr std::string_view kInputFailed = "ST_GeogFromText failed: ";

absl::StatusOr<std::string> GeographyInput(const Arguments& args) {
  return ReadGeography(args.String(0), /*oriented=*/true, kInputFailed);
}

// Read Parquet binary columns directly, retaining GeoParquet's planar edge semantics.
absl::StatusOr<std::string> ParquetWkbInput(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_geography::ReadWkb(args.String(0), "ST_GeogFromWKB failed: ",
                                                          /*oriented=*/true, args.Bool(1)));
  return EncodeGeography(geography);
}

absl::StatusOr<std::optional<std::string>> UnionGeographies(
    const std::vector<std::string>& values) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Geography> geographies, Decode(values));
  auto result = googlesql_geography::UnionGeographies(geographies);
  if (!result.ok()) {
    return result.status();
  }
  return Encode(*result);
}

absl::StatusOr<std::optional<std::string>> CentroidAggregate(
    const std::vector<std::string>& values) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Geography> geographies, Decode(values));
  return Encode(googlesql_geography::CentroidAggregate(geographies));
}

absl::StatusOr<std::string> MakeLine(const std::vector<std::string>& values) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Geography> geographies, Decode(values));
  auto result = googlesql_geography::MakeLine(geographies);
  if (!result.ok()) {
    return result.status();
  }
  return Encode(*result);
}

absl::StatusOr<std::string> MakePolygon(const std::vector<std::string>& values,
                                        bool oriented = false) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Geography> geographies, Decode(values));
  auto result = googlesql_geography::MakePolygon(geographies, oriented);
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
    duckdb_functions::SetResult(duckdb_struct_vector_get_child(output, i), row, values.at(i));
  }
}

template <bool kList>
void Bounds(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto& options = *static_cast<const Options*>(duckdb_scalar_function_get_extra_info(info));
  EachRow(
      info, input, output, [&](const Arguments& args) -> absl::StatusOr<std::optional<BoxResult>> {
        std::optional<BoundingBox> box;
        if constexpr (kList) {
          GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Geography> geographies,
                                     Decode(args.Strings(0)));
          box = googlesql_geography::Extent(geographies, options);
        } else {
          GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, DecodeGeography(args.String(0)));
          box = googlesql_geography::Bounds(geography, options);
        }
        if (!box) {
          return std::nullopt;
        }
        return BoxResult{.box = *box};
      });
}

// The bounds of a GEOGRAPHY, or with `kList` of a list of them. The registered function owns a
// copy, so caller options may be temporary and different databases can use different policies
// without sharing mutable state.
template <bool kList>
void RegisterBounds(duckdb_connection connection, const char* name, const Options& options) {
  const LogicalType geography(duckdb_create_logical_type(kGeography));
  const LogicalType list(duckdb_create_list_type(geography.get()));
  const LogicalType dbl(duckdb_create_logical_type(kDouble));
  std::array<duckdb_logical_type, 4> fields = {dbl.get(), dbl.get(), dbl.get(), dbl.get()};
  std::array<const char*, 4> names = {"xmin", "ymin", "xmax", "ymax"};
  const LogicalType box(duckdb_create_struct_type(fields.data(), names.data(), fields.size()));
  const Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), name);
  duckdb_scalar_function_add_parameter(scalar.get(), kList ? list.get() : geography.get());
  duckdb_scalar_function_set_return_type(scalar.get(), box.get());
  duckdb_scalar_function_set_function(scalar.get(), Bounds<kList>);
  duckdb_scalar_function_set_extra_info(scalar.get(), new Options(options),
                                        [](void* data) { delete static_cast<Options*>(data); });
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register " + std::string(name));
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
    duckdb_functions::SetResult(output, row, VectorString(input, row));
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

using function_catalog::AggregateRule;
using function_catalog::TypeUse;
using function_catalog::TypeUses;

struct BackendDefinition {
  const char* name;
  std::function<void(duckdb_connection, const char*, const Options&)> install;
};

struct Definitions {
  std::vector<BackendDefinition> backends;

  void Custom(const char* name,
              std::function<void(duckdb_connection, const char*, const Options&)> install) {
    backends.push_back({.name = name, .install = std::move(install)});
  }

  void Scalar(const char* name, const std::vector<duckdb_type>& parameters, duckdb_type result,
              duckdb_scalar_function_t implementation) {
    Custom(name,
           [parameters = parameters, result, implementation](
               duckdb_connection connection, const char* backend, const Options& /*options*/) {
             Register(connection, backend, parameters, result, implementation);
           });
  }
};

const Definitions& GeographyDefinitions() {
  static const auto* const kDefinitions = [] {
    auto* definitions = new Definitions();
    definitions->Scalar("bq_geography_text", {kGeography}, kVarchar,
                        EachRow<OfGeography<GeographyText>>);
    definitions->Scalar("bq_st_geogpoint", {kDouble, kDouble}, kGeography,
                        EachRow<Call<googlesql_geography::GeogPoint>>);
    definitions->Scalar("bq_st_geogfromtext", {kVarchar, kBoolean, kBoolean, kBoolean}, kGeography,
                        EachRow<GeogFromText>);
    definitions->Scalar("bq_st_geogfromgeojson", {kVarchar, kBoolean}, kGeography,
                        EachRow<GeogFromGeoJson>);
    // ST_GEOGFROMWKB and ST_GEOGFROM read BYTES as WKB, and ST_GEOGFROMWKB reads a STRING as its
    // hex and ST_GEOGFROM as WKT, GeoJSON or hex WKB.
    {
      definitions->Scalar("bq_st_geogfromwkb", {kBlob}, kGeography, EachRow<GeogFromWkb>);
    }
    {
      definitions->Scalar("bq_st_geogfrombytes", {kBlob}, kGeography, EachRow<GeogFromBytes>);
    }
    {
      definitions->Scalar("bq_st_geogfromhexwkb", {kVarchar}, kGeography, EachRow<GeogFromHexWkb>);
    }
    {
      definitions->Scalar("bq_st_geogfrom", {kVarchar}, kGeography, EachRow<GeogFrom>);
    }
    {
      definitions->Scalar("bq_st_geogfromwkb_oriented", {kBlob, kBoolean, kBoolean, kBoolean},
                          kGeography, EachRow<GeogFromOrientedWkb<false>>);
    }
    {
      definitions->Scalar("bq_st_geogfromhexwkb_oriented", {kVarchar, kBoolean, kBoolean, kBoolean},
                          kGeography, EachRow<GeogFromOrientedWkb<true>>);
    }
    definitions->Scalar("bq_geography_input", {kVarchar}, kGeography, EachRow<GeographyInput>);
    definitions->Scalar("bq_parquet_wkb_input", {kBlob, kBoolean}, kGeography,
                        EachRow<ParquetWkbInput>);
    definitions->Scalar("bq_st_geogpointfromgeohash", {kVarchar}, kGeography,
                        EachRow<Call<googlesql_geography::PointFromGeohash>>);
    definitions->Scalar("bq_st_astext", {kGeography}, kVarchar,
                        EachRow<OfGeography<[](const Geography& geography) {
                          return googlesql_geography::Wkt(geography);
                        }>>);
    definitions->Scalar("bq_st_asbinary", {kGeography}, kBlob,
                        EachRow<OfGeography<googlesql_geography::Wkb>>);
    definitions->Scalar("bq_st_asgeojson", {kGeography}, kVarchar,
                        EachRow<Call<googlesql_geography::AsGeoJson>>);
    definitions->Scalar("bq_st_angle", {kGeography, kGeography, kGeography}, kDouble,
                        EachRow<Call<googlesql_geography::Angle>>);
    definitions->Scalar("bq_st_centroid", {kGeography}, kGeography,
                        EachRow<Call<googlesql_geography::Centroid>>);
    definitions->Scalar("bq_st_convexhull", {kGeography}, kGeography,
                        EachRow<Call<googlesql_geography::ConvexHull>>);
    definitions->Scalar("bq_st_lineinterpolatepoint", {kGeography, kDouble}, kGeography,
                        EachRow<Call<googlesql_geography::LineInterpolatePoint>>);
    definitions->Scalar("bq_st_linelocatepoint", {kGeography, kGeography}, kDouble,
                        EachRow<Call<googlesql_geography::LineLocatePoint>>);
    definitions->Scalar("bq_st_linesubstring", {kGeography, kDouble, kDouble}, kGeography,
                        EachRow<Call<googlesql_geography::LineSubstring>>);
    definitions->Scalar("bq_st_azimuth", {kGeography, kGeography}, kDouble,
                        EachRow<Call<googlesql_geography::Azimuth>>);
    definitions->Scalar("bq_st_distance", {kGeography, kGeography, kBoolean, kVarchar}, kDouble,
                        EachRow<Call<googlesql_geography::Distance>>);
    definitions->Scalar("bq_st_hausdorffdistance", {kGeography, kGeography, kBoolean}, kDouble,
                        EachRow<Call<googlesql_geography::HausdorffDistance>>);
    {
      definitions->Scalar("bq_st_hausdorffdwithin", {kGeography, kGeography, kBoolean, kDouble},
                          kBoolean, EachRow<Call<googlesql_geography::HausdorffDWithin>>);
    }
    definitions->Scalar("bq_st_closestpoint", {kGeography, kGeography, kBoolean, kVarchar},
                        kGeography, EachRow<Call<googlesql_geography::ClosestPoint>>);
    definitions->Scalar("bq_st_intersectsbox", {kGeography, kDouble, kDouble, kDouble, kDouble},
                        kBoolean, EachRow<Call<googlesql_geography::IntersectsBox>>);
    definitions->Scalar("bq_st_dwithin", {kGeography, kGeography, kDouble, kBoolean, kVarchar},
                        kBoolean, EachRow<Call<googlesql_geography::DWithin>>);
    definitions->Scalar("bq_st_equals", {kGeography, kGeography}, kBoolean,
                        EachRow<Call<googlesql_geography::Equals>>);
    {
      definitions->Scalar("bq_st_intersects", {kGeography, kGeography}, kBoolean,
                          EachRow<Call<googlesql_geography::Intersects>>);
    }
    definitions->Scalar("bq_st_intersection", {kGeography, kGeography}, kGeography,
                        EachRow<SetOperation<S2BooleanOperation::OpType::INTERSECTION>>);
    definitions->Scalar("bq_st_difference", {kGeography, kGeography}, kGeography,
                        EachRow<SetOperation<S2BooleanOperation::OpType::DIFFERENCE>>);
    definitions->Scalar("bq_st_boundary", {kGeography}, kGeography,
                        EachRow<Call<googlesql_geography::Boundary>>);
    definitions->Scalar("bq_st_buffer",
                        {kGeography, kDouble, kDouble, kBoolean, kVarchar, kVarchar}, kGeography,
                        EachRow<Call<googlesql_geography::Buffer>>);
    definitions->Scalar("bq_st_bufferwithtolerance",
                        {kGeography, kDouble, kDouble, kBoolean, kVarchar, kVarchar}, kGeography,
                        EachRow<Call<googlesql_geography::BufferWithTolerance>>);
    definitions->Scalar("bq_st_simplify", {kGeography, kDouble}, kGeography,
                        EachRow<Call<googlesql_geography::Simplify>>);
    definitions->Scalar("bq_st_snaptogrid", {kGeography, kDouble}, kGeography,
                        EachRow<Call<googlesql_geography::SnapToGrid>>);
    definitions->Scalar("bq_st_touches", {kGeography, kGeography}, kBoolean,
                        EachRow<Call<googlesql_geography::Touches>>);
    {
      definitions->Scalar("bq_st_covers", {kGeography, kGeography}, kBoolean,
                          EachRow<Call<googlesql_geography::Covers>>);
    }
    {
      definitions->Scalar("bq_st_contains", {kGeography, kGeography}, kBoolean,
                          EachRow<Call<googlesql_geography::Contains>>);
    }
    definitions->Scalar("bq_st_x", {kGeography}, kDouble, EachRow<Call<googlesql_geography::X>>);
    definitions->Scalar("bq_st_y", {kGeography}, kDouble, EachRow<Call<googlesql_geography::Y>>);
    definitions->Scalar("bq_st_geometrytype", {kGeography}, kVarchar,
                        EachRow<OfGeography<googlesql_geography::GeometryType>>);
    definitions->Scalar("bq_st_dimension", {kGeography}, kBigint,
                        EachRow<OfGeography<googlesql_geography::Dimension>>);
    definitions->Scalar("bq_st_numpoints", {kGeography}, kBigint,
                        EachRow<OfGeography<googlesql_geography::NumPoints>>);
    definitions->Scalar(
        "bq_st_isempty", {kGeography}, kBoolean,
        EachRow<OfGeography<[](const Geography& geography) { return geography.Empty(); }>>);
    definitions->Scalar(
        "bq_st_iscollection", {kGeography}, kBoolean,
        EachRow<OfGeography<[](const Geography& geography) { return geography.Count() > 1; }>>);
    definitions->Scalar("bq_st_numgeometries", {kGeography}, kBigint,
                        EachRow<OfGeography<[](const Geography& geography) {
                          return static_cast<int64_t>(geography.Count());
                        }>>);
    definitions->Scalar("bq_st_isclosed", {kGeography}, kBoolean,
                        EachRow<OfGeography<googlesql_geography::IsClosed>>);
    definitions->Scalar("bq_st_isring", {kGeography}, kBoolean,
                        EachRow<OfGeography<googlesql_geography::IsRing>>);
    definitions->Scalar("bq_st_length", {kGeography, kBoolean, kVarchar}, kDouble,
                        EachRow<Call<googlesql_geography::Length>>);
    definitions->Scalar("bq_st_measure", {kGeography, kBoolean, kVarchar}, kDouble,
                        EachRow<Call<googlesql_geography::Measure>>);
    definitions->Scalar("bq_st_exteriorring", {kGeography}, kGeography,
                        EachRow<Call<googlesql_geography::ExteriorRing>>);
    definitions->Scalar(
        "bq_st_makepolygon", {kGeography}, kGeography,
        EachRow<[](const Arguments& args) { return MakePolygon({args.String(0)}); }>);
    {
      definitions->Scalar("bq_st_pointn", {kGeography, kBigint}, kGeography,
                          EachRow<Call<googlesql_geography::PointN>>);
    }
    definitions->Scalar(
        "bq_st_makeline", {kGeography, kGeography}, kGeography,
        EachRow<[](const Arguments& args) { return MakeLine({args.String(0), args.String(1)}); }>);
    definitions->Scalar("bq_st_union", {kGeography, kGeography}, kGeography,
                        EachRow<[](const Arguments& args) {
                          return UnionGeographies({args.String(0), args.String(1)});
                        }>);
    {
      definitions->Custom(kUnionArray, [](duckdb_connection connection, const char* name,
                                          const Options& /*options*/) {
        const LogicalType geography(duckdb_create_logical_type(kGeography));
        const LogicalType list(duckdb_create_list_type(geography.get()));
        Register(connection, name, {list.get()}, geography.get(),
                 EachRow<[](const Arguments& args) { return UnionGeographies(args.Strings(0)); }>);
      });
    }
    {
      definitions->Custom("bq_st_makeline_array", [](duckdb_connection connection, const char* name,
                                                     const Options& /*options*/) {
        const LogicalType geography(duckdb_create_logical_type(kGeography));
        const LogicalType list(duckdb_create_list_type(geography.get()));
        Register(connection, name, {list.get()}, geography.get(),
                 EachRow<[](const Arguments& args) { return MakeLine(args.Strings(0)); }>);
      });
    }
    {
      definitions->Custom("bq_st_makepolygon_holes", [](duckdb_connection connection,
                                                        const char* name,
                                                        const Options& /*options*/) {
        const LogicalType geography(duckdb_create_logical_type(kGeography));
        const LogicalType list(duckdb_create_list_type(geography.get()));
        Register(connection, name, std::vector<duckdb_logical_type>{geography.get(), list.get()},
                 geography.get(), EachRow<[](const Arguments& args) {
                   std::vector<std::string> rings = args.Strings(1);
                   rings.insert(rings.begin(), args.String(0));
                   return MakePolygon(rings);
                 }>);
      });
    }
    {
      definitions->Custom(
          "bq_st_makepolygonoriented",
          [](duckdb_connection connection, const char* name, const Options& /*options*/) {
            const LogicalType geography(duckdb_create_logical_type(kGeography));
            const LogicalType list(duckdb_create_list_type(geography.get()));
            Register(connection, name, {list.get()}, geography.get(),
                     EachRow<[](const Arguments& args) {
                       return MakePolygon(args.Strings(0), /*oriented=*/true);
                     }>);
          });
    }
    {
      definitions->Custom("bq_st_dump", [](duckdb_connection connection, const char* name,
                                           const Options& /*options*/) {
        const LogicalType geography(duckdb_create_logical_type(kGeography));
        const LogicalType list(duckdb_create_list_type(geography.get()));
        const LogicalType bigint(duckdb_create_logical_type(kBigint));
        Register(connection, name, std::vector<duckdb_logical_type>{geography.get(), bigint.get()},
                 list.get(), EachRow<Call<googlesql_geography::Dump>>);
      });
    }
    definitions->Custom("bq_st_interiorrings", [](duckdb_connection connection, const char* name,
                                                  const Options& /*options*/) {
      const LogicalType geography(duckdb_create_logical_type(kGeography));
      const LogicalType list(duckdb_create_list_type(geography.get()));
      Register(connection, name, {geography.get()}, list.get(),
               EachRow<Call<googlesql_geography::InteriorRings>>);
    });
    definitions->Scalar("bq_st_geohash", {kGeography, kBigint}, kVarchar,
                        EachRow<Call<googlesql_geography::Geohash>>);
    definitions->Scalar("bq_s2_cellidfrompoint", {kGeography, kBigint}, kBigint,
                        EachRow<Call<googlesql_geography::CellIdFromPoint>>);
    definitions->Custom("bq_s2_coveringcellids", [](duckdb_connection connection, const char* name,
                                                    const Options& /*options*/) {
      const LogicalType geography(duckdb_create_logical_type(kGeography));
      const LogicalType bigint(duckdb_create_logical_type(kBigint));
      const LogicalType number(duckdb_create_logical_type(kDouble));
      const LogicalType list(duckdb_create_list_type(bigint.get()));
      Register(connection, name,
               std::vector<duckdb_logical_type>{
                   geography.get(),
                   bigint.get(),
                   bigint.get(),
                   bigint.get(),
                   number.get(),
               },
               list.get(), EachRow<Call<googlesql_geography::CoveringCellIds>>);
    });
    definitions->Custom("bq_st_boundingbox", RegisterBounds<false>);
    // The aggregates' finishes, over the non-NULL values that list() collects.
    definitions->Custom(kCentroidAggregate, [](duckdb_connection connection, const char* name,
                                               const Options& /*options*/) {
      const LogicalType geography(duckdb_create_logical_type(kGeography));
      const LogicalType list(duckdb_create_list_type(geography.get()));
      Register(connection, name, {list.get()}, geography.get(),
               EachRow<[](const Arguments& args) { return CentroidAggregate(args.Strings(0)); }>);
    });
    definitions->Custom("bq_st_clusterdbscan", [](duckdb_connection connection, const char* name,
                                                  const Options& /*options*/) {
      const LogicalType geography(duckdb_create_logical_type(kGeography));
      const LogicalType list(duckdb_create_list_type(geography.get()));
      const LogicalType bigint(duckdb_create_logical_type(kBigint));
      const LogicalType number(duckdb_create_logical_type(kDouble));
      const LogicalType result(duckdb_create_list_type(bigint.get()));
      Register(
          connection, name,
          std::vector<duckdb_logical_type>{list.get(), number.get(), bigint.get()}, result.get(),
          EachRow<[](const Arguments& args) -> absl::StatusOr<std::vector<std::optional<int64_t>>> {
            const auto entry = VectorElement<duckdb_list_entry>(args.Vector(0), args.Row());
            auto* const child = duckdb_list_vector_get_child(args.Vector(0));
            std::vector<IndexedGeography> values;
            values.reserve(entry.length);
            for (idx_t i = 0; i < entry.length; ++i) {
              const auto row = entry.offset + i;
              auto* validity = duckdb_vector_get_validity(child);
              if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
                // NULL and empty inputs are both noise and contribute no neighbors.
                values.emplace_back(Geography{});
                continue;
              }
              GOOGLESQL_ASSIGN_OR_RETURN(auto value,
                                         DecodeIndexedGeography(VectorString(child, row)));
              values.push_back(std::move(value));
            }
            return googlesql_geography::ClusterDBSCAN(values, args.Double(1), args.Int(2));
          }>);
    });
    definitions->Custom(kExtent, RegisterBounds<true>);
    return definitions;
  }();
  return *kDefinitions;
}

}  // namespace

std::span<const function_catalog::StaticNamedDefinition<function_catalog::StaticRule>>
GeographyFunctions() {
  static constexpr TypeUses kUses = {TypeUse::kBuildsOrReadsGeography};
  static constexpr googlesql::TypeKind kBytes[] = {googlesql::TYPE_BYTES};
  static constexpr googlesql::TypeKind kString[] = {googlesql::TYPE_STRING};
  static constexpr function_catalog::StaticCondition kBytesArgument[] = {
      {.argument = 1, .types = kBytes},
  };
  static constexpr function_catalog::StaticCondition kStringArgument[] = {
      {.argument = 1, .types = kString},
  };
  static constexpr std::string_view kFalse[] = {"false"};
  static constexpr std::string_view kFromTextDefaults[] = {"false", "false", "false"};
  static constexpr std::string_view kGeohashDefaults[] = {"20"};
  static constexpr std::string_view kCellIdDefaults[] = {"30"};
  static constexpr std::string_view kBufferDefaults[] = {"8.0", "false", "'ROUND'", "'BOTH'"};
  static constexpr std::string_view kBufferWithToleranceDefaults[] = {"false", "'ROUND'", "'BOTH'"};
  static constexpr std::string_view kCoveringDefaults[] = {"0", "30", "8", "0.0"};
  static constexpr auto kRulesBacking = std::tuple{
      Define("ST_GEOGPOINT", {{.arity = 2, .spelling = "bq_st_geogpoint($1, $2)"}},
             {.uses = kUses}),
      Define("ST_GEOGFROMTEXT",
             {
                 {
                     .arity = {1, 4},
                     .spelling = "bq_st_geogfromtext($1, $2, $3, $4)",
                     .defaults = kFromTextDefaults,
                 },
             },
             {.uses = kUses}),
      Define("ST_GEOGFROMGEOJSON",
             {
                 {.arity = 1, .spelling = "bq_st_geogfromgeojson($1, FALSE)"},
                 {.arity = 2, .spelling = "bq_st_geogfromgeojson($1, $2)"},
             },
             {.uses = kUses}),
      Define("ST_GEOGPOINTFROMGEOHASH",
             {{.arity = 1, .spelling = "bq_st_geogpointfromgeohash($1)"}}, {.uses = kUses}),
      Define("ST_ASTEXT", {{.arity = 1, .spelling = "bq_st_astext($1)"}}, {.uses = kUses}),
      Define("ST_ASBINARY", {{.arity = 1, .spelling = "bq_st_asbinary($1)"}}, {.uses = kUses}),
      Define("ST_ASGEOJSON", {{.arity = 1, .spelling = "bq_st_asgeojson($1)"}}, {.uses = kUses}),
      Define("ST_ANGLE", {{.arity = 3, .spelling = "bq_st_angle($1, $2, $3)"}}, {.uses = kUses}),
      Define("ST_CENTROID", {{.arity = 1, .spelling = "bq_st_centroid($1)"}}, {.uses = kUses}),
      Define("ST_CONVEXHULL", {{.arity = 1, .spelling = "bq_st_convexhull($1)"}}, {.uses = kUses}),
      Define("ST_LINEINTERPOLATEPOINT",
             {{.arity = 2, .spelling = "bq_st_lineinterpolatepoint($1, $2)"}}, {.uses = kUses}),
      Define("ST_LINELOCATEPOINT", {{.arity = 2, .spelling = "bq_st_linelocatepoint($1, $2)"}},
             {.uses = kUses}),
      Define("ST_LINESUBSTRING", {{.arity = 3, .spelling = "bq_st_linesubstring($1, $2, $3)"}},
             {.uses = kUses}),
      Define("ST_AZIMUTH", {{.arity = 2, .spelling = "bq_st_azimuth($1, $2)"}}, {.uses = kUses}),
      // The backend takes the public name for its error on use_spheroid = TRUE.
      Define("ST_DISTANCE",
             {
                 {
                     .arity = {2, 3},
                     .spelling = "bq_st_distance($1, $2, $3, 'ST_DISTANCE')",
                     .defaults = kFalse,
                 },
             },
             {.uses = kUses}),
      Define("ST_MAXDISTANCE",
             {
                 {
                     .arity = {2, 3},
                     .spelling = "bq_st_distance($1, $2, $3, 'ST_MAXDISTANCE')",
                     .defaults = kFalse,
                 },
             },
             {.uses = kUses}),
      Define("ST_HAUSDORFFDISTANCE",
             {
                 {
                     .arity = {2, 3},
                     .spelling = "bq_st_hausdorffdistance($1, $2, $3)",
                     .defaults = kFalse,
                 },
             },
             {.uses = kUses}),
      Define("ST_CLOSESTPOINT",
             {
                 {
                     .arity = {2, 3},
                     .spelling = "bq_st_closestpoint($1, $2, $3, 'ST_CLOSESTPOINT')",
                     .defaults = kFalse,
                 },
             },
             {.uses = kUses}),
      Define("ST_INTERSECTSBOX",
             {{.arity = 5, .spelling = "bq_st_intersectsbox($1, $2, $3, $4, $5)"}},
             {.uses = kUses}),
      Define("ST_DWITHIN",
             {
                 {
                     .arity = {3, 4},
                     .spelling = "bq_st_dwithin($1, $2, $3, $4, 'ST_DWITHIN')",
                     .defaults = kFalse,
                 },
             },
             {.uses = kUses}),
      Define("ST_EQUALS", {{.arity = 2, .spelling = "bq_st_equals($1, $2)"}}, {.uses = kUses}),
      Define("ST_INTERSECTS", {{.arity = 2, .spelling = "bq_st_intersects($1, $2)"}},
             {.uses = kUses}),
      Define("ST_INTERSECTION", {{.arity = 2, .spelling = "bq_st_intersection($1, $2)"}},
             {.uses = kUses}),
      Define("ST_DIFFERENCE", {{.arity = 2, .spelling = "bq_st_difference($1, $2)"}},
             {.uses = kUses}),
      Define("ST_BOUNDARY", {{.arity = 1, .spelling = "bq_st_boundary($1)"}}, {.uses = kUses}),
      Define("ST_BUFFER",
             {
                 {
                     .arity = {2, 6},
                     .spelling = "bq_st_buffer($1, $2, $3, $4, $5, $6)",
                     .defaults = kBufferDefaults,
                 },
             },
             {.uses = kUses}),
      Define("ST_BUFFERWITHTOLERANCE",
             {
                 {
                     .arity = {3, 6},
                     .spelling = "bq_st_bufferwithtolerance($1, $2, $3, $4, $5, $6)",
                     .defaults = kBufferWithToleranceDefaults,
                 },
             },
             {.uses = kUses}),
      Define("ST_SIMPLIFY", {{.arity = 2, .spelling = "bq_st_simplify($1, $2)"}}, {.uses = kUses}),
      Define("ST_SNAPTOGRID", {{.arity = 2, .spelling = "bq_st_snaptogrid($1, $2)"}},
             {.uses = kUses}),
      Define("ST_TOUCHES", {{.arity = 2, .spelling = "bq_st_touches($1, $2)"}}, {.uses = kUses}),
      Define("ST_COVERS", {{.arity = 2, .spelling = "bq_st_covers($1, $2)"}}, {.uses = kUses}),
      Define("ST_CONTAINS", {{.arity = 2, .spelling = "bq_st_contains($1, $2)"}}, {.uses = kUses}),
      Define("ST_X", {{.arity = 1, .spelling = "bq_st_x($1)"}}, {.uses = kUses}),
      Define("ST_Y", {{.arity = 1, .spelling = "bq_st_y($1)"}}, {.uses = kUses}),
      Define("ST_GEOMETRYTYPE", {{.arity = 1, .spelling = "bq_st_geometrytype($1)"}},
             {.uses = kUses}),
      Define("ST_DIMENSION", {{.arity = 1, .spelling = "bq_st_dimension($1)"}}, {.uses = kUses}),
      Define("ST_NUMPOINTS", {{.arity = 1, .spelling = "bq_st_numpoints($1)"}}, {.uses = kUses}),
      Define("ST_NPOINTS", {{.arity = 1, .spelling = "bq_st_numpoints($1)"}}, {.uses = kUses}),
      Define("ST_ISEMPTY", {{.arity = 1, .spelling = "bq_st_isempty($1)"}}, {.uses = kUses}),
      Define("ST_ISCOLLECTION", {{.arity = 1, .spelling = "bq_st_iscollection($1)"}},
             {.uses = kUses}),
      Define("ST_NUMGEOMETRIES", {{.arity = 1, .spelling = "bq_st_numgeometries($1)"}},
             {.uses = kUses}),
      Define("ST_ISCLOSED", {{.arity = 1, .spelling = "bq_st_isclosed($1)"}}, {.uses = kUses}),
      Define("ST_ISRING", {{.arity = 1, .spelling = "bq_st_isring($1)"}}, {.uses = kUses}),
      Define("ST_LENGTH",
             {
                 {
                     .arity = {1, 2},
                     .spelling = "bq_st_length($1, $2, 'ST_LENGTH')",
                     .defaults = kFalse,
                 },
             },
             {.uses = kUses}),
      Define("ST_AREA",
             {
                 {
                     .arity = {1, 2},
                     .spelling = "bq_st_measure($1, $2, 'ST_AREA')",
                     .defaults = kFalse,
                 },
             },
             {.uses = kUses}),
      Define("ST_PERIMETER",
             {
                 {
                     .arity = {1, 2},
                     .spelling = "bq_st_measure($1, $2, 'ST_PERIMETER')",
                     .defaults = kFalse,
                 },
             },
             {.uses = kUses}),
      Define("ST_EXTERIORRING", {{.arity = 1, .spelling = "bq_st_exteriorring($1)"}},
             {.uses = kUses}),
      Define("ST_POINTN", {{.arity = 2, .spelling = "bq_st_pointn($1, $2)"}}, {.uses = kUses}),
      Define("ST_INTERIORRINGS", {{.arity = 1, .spelling = "bq_st_interiorrings($1)"}},
             {.uses = kUses}),
      Define("ST_GEOHASH",
             {
                 {
                     .arity = {1, 2},
                     .spelling = "bq_st_geohash($1, $2)",
                     .defaults = kGeohashDefaults,
                 },
             },
             {.uses = {TypeUse::kBuildsOrReadsGeography, TypeUse::kRejectsSafe}}),
      Define("S2_CELLIDFROMPOINT",
             {
                 {
                     .arity = {1, 2},
                     .spelling = "bq_s2_cellidfrompoint($1, $2)",
                     .defaults = kCellIdDefaults,
                 },
             },
             {.uses = kUses}),
      Define("ST_BOUNDINGBOX", {{.arity = 1, .spelling = "bq_st_boundingbox($1)"}},
             {.uses = kUses}),
      Define(
          "ST_GEOGFROMWKB",
          {
              {.arity = 1, .spelling = "bq_st_geogfromwkb($1)", .conditions = kBytesArgument},
              {.arity = 1, .spelling = "bq_st_geogfromhexwkb($1)", .conditions = kStringArgument},
              {
                  .arity = 4,
                  .spelling = "bq_st_geogfromwkb_oriented($1, $2, $3, $4)",
                  .conditions = kBytesArgument,
              },
              {
                  .arity = 4,
                  .spelling = "bq_st_geogfromhexwkb_oriented($1, $2, $3, $4)",
                  .conditions = kStringArgument,
              },
          },
          {.uses = kUses}),
      Define("ST_GEOGFROM",
             {
                 {.arity = 1, .spelling = "bq_st_geogfrombytes($1)", .conditions = kBytesArgument},
                 {.arity = 1, .spelling = "bq_st_geogfrom($1)", .conditions = kStringArgument},
             },
             {.uses = kUses}),
      Define("ST_HAUSDORFFDWITHIN",
             {
                 {
                     .arity = {3, 4},
                     .spelling = "bq_st_hausdorffdwithin($1, $2, $4, $3)",
                     .defaults = kFalse,
                 },
             },
             {.uses = kUses}),
      Define("ST_DISJOINT", {{.arity = 2, .spelling = "(NOT bq_st_intersects($1, $2))"}},
             {.uses = kUses}),
      Define("ST_COVEREDBY", {{.arity = 2, .spelling = "bq_st_covers($2, $1)"}}, {.uses = kUses}),
      Define("ST_WITHIN", {{.arity = 2, .spelling = "bq_st_contains($2, $1)"}}, {.uses = kUses}),
      Define("ST_STARTPOINT", {{.arity = 1, .spelling = "bq_st_pointn($1, 1)"}}, {.uses = kUses}),
      Define("ST_ENDPOINT", {{.arity = 1, .spelling = "bq_st_pointn($1, -1)"}}, {.uses = kUses}),
      Define(
          "ST_UNION",
          {
              {.arity = 2, .spelling = "bq_st_union($1, $2)"},
              {
                  .arity = 1,
                  .spelling =
                      "bq_st_union_array(list_filter($1, lambda geography: geography IS NOT NULL))",
              },
          },
          {.uses = kUses}),
      Define("ST_MAKELINE",
             {
                 {.arity = 2, .spelling = "bq_st_makeline($1, $2)"},
                 {
                     .arity = 1,
                     .spelling = "CASE WHEN list_count($1) <> len($1) THEN NULL ELSE "
                                 "bq_st_makeline_array($1) END",
                 },
             },
             {.uses = kUses}),
      Define("ST_MAKEPOLYGON",
             {
                 {.arity = 1, .spelling = "bq_st_makepolygon($1)"},
                 {
                     .arity = 2,
                     .spelling = "CASE WHEN list_count($2) <> len($2) THEN NULL ELSE "
                                 "bq_st_makepolygon_holes($1, $2) END",
                 },
             },
             {.uses = kUses}),
      Define(
          "ST_MAKEPOLYGONORIENTED",
          {
              {
                  .arity = 1,
                  .spelling = "CASE WHEN len($1) = 0 OR list_count($1) <> len($1) THEN NULL ELSE "
                              "bq_st_makepolygonoriented($1) END",
              },
          },
          {.uses = kUses}),
      Define("ST_DUMP",
             {
                 {.arity = 1, .spelling = "bq_st_dump($1, -1)"},
                 {
                     .arity = 2,
                     .spelling = "CASE WHEN $2 IS NULL THEN CASE WHEN $1 IS NULL THEN "
                                 "NULL ELSE []::BLOB[] END ELSE bq_st_dump($1, $2) END",
                 },
             },
             {.uses = kUses}),
      Define(
          "S2_COVERINGCELLIDS",
          {
              {
                  .arity = {1, 5},
                  .spelling = "coalesce(bq_s2_coveringcellids($1, $2, $3, $4, $5), []::BIGINT[])",
                  .defaults = kCoveringDefaults,
              },
          },
          {.uses = {TypeUse::kBuildsOrReadsGeography, TypeUse::kRejectsSafe}}),
  };
  static constexpr auto kRules = DefinitionViews(kRulesBacking);
  return kRules;
}

namespace {

// An aggregate of the non-NULL values that list() collects, which the registered `kFunction`
// finishes. ST_UNION_AGG unions them as ST_UNION does an array, which puts all the points first,
// so none of these folds the values one at a time.
template <const char* kFunction>
constexpr function_catalog::StaticAggregateRule kListAggregate = {
    .function = "list",
    .distinct = AggregateRule::Distinct::kUnsupported,
    .skip_nulls = true,
    .finish = [](const std::string& sql, const std::vector<std::string>& /*arguments*/,
                 const std::string& /*tail*/) { return std::string(kFunction) + "(" + sql + ")"; },
};

// BigQuery takes none of them over a window.
constexpr TypeUses kAggregateUses = {TypeUse::kBuildsOrReadsGeography, TypeUse::kRejectsOver};

}  // namespace

std::span<const function_catalog::StaticNamedDefinition<function_catalog::StaticAggregateRule>>
GeographyAggregates() {
  static constexpr auto kAggregatesBacking = std::tuple{

      Define("ST_UNION_AGG", {kListAggregate<kUnionArray>},
             {
                 .uses = kAggregateUses,
             }),

      Define("ST_CENTROID_AGG", {kListAggregate<kCentroidAggregate>},
             {
                 .uses = kAggregateUses,
             }),

      Define("ST_EXTENT", {kListAggregate<kExtent>},
             {
                 .uses = kAggregateUses,
             }),
  };
  static constexpr auto kAggregates = DefinitionViews(kAggregatesBacking);
  return kAggregates;
}

std::span<const function_catalog::StaticNamedDefinition<function_catalog::StaticAggregateRule>>
GeographyAnalytics() {
  static constexpr std::string_view kValue[] = {"$1"};
  static constexpr auto kBacking = std::tuple{
      Define("ST_CLUSTERDBSCAN",
             {
                 {
                     .function = "list",
                     .type = googlesql::TYPE_GEOGRAPHY,
                     .arguments = kValue,
                     .distinct = AggregateRule::Distinct::kUnsupported,
                     .finish =
                         [](const std::string& sql, const std::vector<std::string>& arguments,
                            const std::string& /*tail*/) {
                           return "bq_st_clusterdbscan(" + sql + ", " + arguments.at(1) + ", " +
                                  arguments.at(2) + ")";
                         },
                     .partition_results = true,
                 },
             },
             {.uses = {TypeUse::kBuildsOrReadsGeography, TypeUse::kRejectsSafe}}),
  };
  static constexpr auto kDefinitions = DefinitionViews(kBacking);
  return kDefinitions;
}

void RegisterGeographyFunctions(duckdb_connection connection,
                                const googlesql_geography::Options& options) {
  RegisterGeographyStorage(connection);
  for (const BackendDefinition& backend : GeographyDefinitions().backends) {
    backend.install(connection, backend.name, options);
  }
}

}  // namespace bigquery_emulator_duckdb::geography_duckdb
