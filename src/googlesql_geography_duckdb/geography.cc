#include "src/googlesql_geography_duckdb/geography.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/type.pb.h"
#include "s2/s2boolean_operation.h"
#include "src/backend_error.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/duckdb_sql.h"
#include "src/function_catalog/rules.h"
#include "src/googlesql_geography/geography.h"
#include "src/googlesql_geography/geojson.h"
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
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_geography::ReadWkt(args.String(0)));
  return EncodeGeography(geography);
}

absl::StatusOr<std::string> GeogFromGeoJson(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_geography::ReadGeoJson(args.String(0)));
  return EncodeGeography(geography);
}

absl::StatusOr<std::string> GeogFromWkb(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_geography::ReadWkb(args.String(0)));
  return EncodeGeography(geography);
}

// The geography of hex-encoded WKB, whose odd first digit BigQuery reads as a byte of its own.
absl::StatusOr<Geography> ReadHexWkb(std::string_view text, std::string_view failed,
                                     bool oriented) {
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
  return googlesql_geography::ReadWkb(bytes, failed, oriented);
}

absl::StatusOr<std::string> GeogFromHexWkb(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const Geography geography,
      ReadHexWkb(args.String(0), "ST_GeogFromWKBHex failed: ", /*oriented=*/false));
  return EncodeGeography(geography);
}

// The fewest hex digits ST_GEOGFROM reads as WKB, those of its byte order and type.
constexpr size_t kWkbHeaderDigits = 10;

// The geography of `text` as ST_GEOGFROM reads a STRING, and loads and tabledata.insertAll with
// `oriented` WKT and WKB: GeoJSON where it starts with '{', hex-encoded WKB where it has only hex
// digits, enough for a WKB header, and starts with a byte order, and WKT otherwise. `failed`
// prefixes WKT and WKB errors.
absl::StatusOr<std::string> ReadGeography(const std::string& text, bool oriented,
                                          std::string_view failed) {
  const size_t start = text.find_first_not_of(" \t\r\n");
  if (start != std::string::npos && text.at(start) == '{') {
    GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, googlesql_geography::ReadGeoJson(text));
    return EncodeGeography(geography);
  }
  if (text.size() >= kWkbHeaderDigits && (text.starts_with("00") || text.starts_with("01")) &&
      std::ranges::all_of(text, [](unsigned char c) { return std::isxdigit(c) != 0; })) {
    GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ReadHexWkb(text, failed, oriented));
    return EncodeGeography(geography);
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_geography::ReadWkt(text, oriented, failed));
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

// A GEOGRAPHY that a Parquet load writes, as GeographyInput takes it, except that a binary column
// holds WKB, which DuckDB casts to text that escapes as \xHH each byte it does not print, as it
// does the first byte of any WKB.
absl::StatusOr<std::string> ParquetGeographyInput(const Arguments& args) {
  const std::string text = args.String(0);
  if (!text.starts_with("\\x")) {
    return ReadGeography(text, /*oriented=*/true, kInputFailed);
  }
  std::string bytes;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text.at(i) == '\\' && i + 3 < text.size() && text.at(i + 1) == 'x') {
      std::string byte;
      if (!absl::HexStringToBytes(std::string_view(text).substr(i + 2, 2), &byte)) {
        return absl::InvalidArgumentError("Invalid escaped BLOB text: " + text);
      }
      bytes += byte;
      i += 3;
    } else {
      bytes.push_back(text.at(i));
    }
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography,
                             googlesql_geography::ReadWkb(bytes, "ST_GeogFromWKB failed: ",
                                                          /*oriented=*/true));
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

absl::StatusOr<std::string> MakePolygon(const std::vector<std::string>& values) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Geography> geographies, Decode(values));
  auto result = googlesql_geography::MakePolygon(geographies);
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
using function_catalog::Arity;
using function_catalog::FunctionDefinition;
using function_catalog::Rule;
using function_catalog::TypeUse;
using function_catalog::TypeUses;

struct BackendDefinition {
  const char* name;
  std::function<void(duckdb_connection, const char*, const Options&)> install;
  std::vector<std::pair<std::string_view, FunctionDefinition<Rule>>> functions = {};

  [[nodiscard]] std::string Invoke(std::string_view arguments) const {
    return std::string(name) + "(" + std::string(arguments) + ")";
  }

  BackendDefinition& Sql(std::string_view function, std::vector<Rule> rules, TypeUses uses = {}) {
    uses |= {TypeUse::kBuildsOrReadsGeography};
    functions.emplace_back(function,
                           FunctionDefinition<Rule>{.rules = std::move(rules), .uses = uses});
    return *this;
  }

  BackendDefinition& Call(std::string_view function, Arity arity,
                          std::vector<std::string_view> defaults = {}, TypeUses uses = {}) {
    std::string arguments;
    for (size_t i = 1; i <= arity.max; ++i) {
      arguments += (i == 1 ? "$" : ", $") + std::to_string(i);
    }
    return Sql(function,
               {{.arity = arity, .spelling = Invoke(arguments), .defaults = std::move(defaults)}},
               uses);
  }

  // The backend takes the public name for its error on use_spheroid = TRUE.
  BackendDefinition& Spheroid(std::string_view function, Arity arity) {
    std::string arguments;
    for (size_t i = 1; i <= arity.max; ++i) {
      arguments += "$" + std::to_string(i) + ", ";
    }
    return Sql(function, {
                             {
                                 .arity = arity,
                                 .spelling = Invoke(arguments + QuoteLiteral(function)),
                                 .defaults = {"false"},
                             },
                         });
  }
};

struct Definitions {
  std::vector<BackendDefinition> backends;

  BackendDefinition& Custom(
      const char* name,
      std::function<void(duckdb_connection, const char*, const Options&)> install) {
    backends.push_back({.name = name, .install = std::move(install)});
    return backends.back();
  }

  BackendDefinition& Scalar(const char* name, const std::vector<duckdb_type>& parameters,
                            duckdb_type result, duckdb_scalar_function_t implementation) {
    return Custom(
        name, [parameters = parameters, result, implementation](
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
    definitions
        ->Scalar("bq_st_geogpoint", {kDouble, kDouble}, kGeography,
                 EachRow<Call<googlesql_geography::GeogPoint>>)
        .Call("ST_GEOGPOINT", 2);
    definitions->Scalar("bq_st_geogfromtext", {kVarchar}, kGeography, EachRow<GeogFromText>)
        .Call("ST_GEOGFROMTEXT", 1);
    definitions->Scalar("bq_st_geogfromgeojson", {kVarchar}, kGeography, EachRow<GeogFromGeoJson>)
        .Call("ST_GEOGFROMGEOJSON", 1);
    // ST_GEOGFROMWKB and ST_GEOGFROM read BYTES as WKB, and ST_GEOGFROMWKB reads a STRING as its
    // hex and ST_GEOGFROM as WKT, GeoJSON or hex WKB.
    const auto of_type = [](const BackendDefinition& backend, googlesql::TypeKind type) {
      return std::vector<Rule>{
          {
              .arity = 1,
              .spelling = backend.Invoke("$1"),
              .conditions = {{.argument = 1, .types = {type}}},
          },
      };
    };
    {
      auto& backend =
          definitions->Scalar("bq_st_geogfromwkb", {kBlob}, kGeography, EachRow<GeogFromWkb>);
      backend.Sql("ST_GEOGFROMWKB", of_type(backend, googlesql::TYPE_BYTES));
    }
    {
      auto& backend =
          definitions->Scalar("bq_st_geogfrombytes", {kBlob}, kGeography, EachRow<GeogFromBytes>);
      backend.Sql("ST_GEOGFROM", of_type(backend, googlesql::TYPE_BYTES));
    }
    {
      auto& backend = definitions->Scalar("bq_st_geogfromhexwkb", {kVarchar}, kGeography,
                                          EachRow<GeogFromHexWkb>);
      backend.Sql("ST_GEOGFROMWKB", of_type(backend, googlesql::TYPE_STRING));
    }
    {
      auto& backend =
          definitions->Scalar("bq_st_geogfrom", {kVarchar}, kGeography, EachRow<GeogFrom>);
      backend.Sql("ST_GEOGFROM", of_type(backend, googlesql::TYPE_STRING));
    }
    definitions->Scalar("bq_geography_input", {kVarchar}, kGeography, EachRow<GeographyInput>);
    definitions->Scalar("bq_parquet_geography_input", {kVarchar}, kGeography,
                        EachRow<ParquetGeographyInput>);
    definitions
        ->Scalar("bq_st_geogpointfromgeohash", {kVarchar}, kGeography,
                 EachRow<Call<googlesql_geography::PointFromGeohash>>)
        .Call("ST_GEOGPOINTFROMGEOHASH", 1);
    definitions
        ->Scalar("bq_st_astext", {kGeography}, kVarchar,
                 EachRow<OfGeography<[](const Geography& geography) {
                   return googlesql_geography::Wkt(geography);
                 }>>)
        .Call("ST_ASTEXT", 1);
    definitions
        ->Scalar("bq_st_asbinary", {kGeography}, kBlob,
                 EachRow<OfGeography<googlesql_geography::Wkb>>)
        .Call("ST_ASBINARY", 1);
    definitions
        ->Scalar("bq_st_asgeojson", {kGeography}, kVarchar,
                 EachRow<Call<googlesql_geography::AsGeoJson>>)
        .Call("ST_ASGEOJSON", 1);
    definitions
        ->Scalar("bq_st_angle", {kGeography, kGeography, kGeography}, kDouble,
                 EachRow<Call<googlesql_geography::Angle>>)
        .Call("ST_ANGLE", 3);
    definitions
        ->Scalar("bq_st_centroid", {kGeography}, kGeography,
                 EachRow<Call<googlesql_geography::Centroid>>)
        .Call("ST_CENTROID", 1);
    definitions
        ->Scalar("bq_st_convexhull", {kGeography}, kGeography,
                 EachRow<Call<googlesql_geography::ConvexHull>>)
        .Call("ST_CONVEXHULL", 1);
    definitions
        ->Scalar("bq_st_lineinterpolatepoint", {kGeography, kDouble}, kGeography,
                 EachRow<Call<googlesql_geography::LineInterpolatePoint>>)
        .Call("ST_LINEINTERPOLATEPOINT", 2);
    definitions
        ->Scalar("bq_st_linelocatepoint", {kGeography, kGeography}, kDouble,
                 EachRow<Call<googlesql_geography::LineLocatePoint>>)
        .Call("ST_LINELOCATEPOINT", 2);
    definitions
        ->Scalar("bq_st_linesubstring", {kGeography, kDouble, kDouble}, kGeography,
                 EachRow<Call<googlesql_geography::LineSubstring>>)
        .Call("ST_LINESUBSTRING", 3);
    definitions
        ->Scalar("bq_st_azimuth", {kGeography, kGeography}, kDouble,
                 EachRow<Call<googlesql_geography::Azimuth>>)
        .Call("ST_AZIMUTH", 2);
    definitions
        ->Scalar("bq_st_distance", {kGeography, kGeography, kBoolean, kVarchar}, kDouble,
                 EachRow<Call<googlesql_geography::Distance>>)
        .Spheroid("ST_DISTANCE", {2, 3})
        .Spheroid("ST_MAXDISTANCE", {2, 3});
    definitions
        ->Scalar("bq_st_hausdorffdistance", {kGeography, kGeography, kBoolean}, kDouble,
                 EachRow<Call<googlesql_geography::HausdorffDistance>>)
        .Call("ST_HAUSDORFFDISTANCE", {2, 3}, {"false"});
    {
      auto& backend =
          definitions->Scalar("bq_st_hausdorffdwithin", {kGeography, kGeography, kBoolean, kDouble},
                              kBoolean, EachRow<Call<googlesql_geography::HausdorffDWithin>>);
      backend.Sql(
          "ST_HAUSDORFFDWITHIN",
          {{.arity = {3, 4}, .spelling = backend.Invoke("$1, $2, $4, $3"), .defaults = {"false"}}});
    }
    definitions
        ->Scalar("bq_st_closestpoint", {kGeography, kGeography, kBoolean, kVarchar}, kGeography,
                 EachRow<Call<googlesql_geography::ClosestPoint>>)
        .Spheroid("ST_CLOSESTPOINT", {2, 3});
    definitions
        ->Scalar("bq_st_intersectsbox", {kGeography, kDouble, kDouble, kDouble, kDouble}, kBoolean,
                 EachRow<Call<googlesql_geography::IntersectsBox>>)
        .Call("ST_INTERSECTSBOX", 5);
    definitions
        ->Scalar("bq_st_dwithin", {kGeography, kGeography, kDouble, kBoolean, kVarchar}, kBoolean,
                 EachRow<Call<googlesql_geography::DWithin>>)
        .Spheroid("ST_DWITHIN", {3, 4});
    definitions
        ->Scalar("bq_st_equals", {kGeography, kGeography}, kBoolean,
                 EachRow<Call<googlesql_geography::Equals>>)
        .Call("ST_EQUALS", 2);
    {
      auto& backend = definitions
                          ->Scalar("bq_st_intersects", {kGeography, kGeography}, kBoolean,
                                   EachRow<Call<googlesql_geography::Intersects>>)
                          .Call("ST_INTERSECTS", 2);
      backend.Sql("ST_DISJOINT",
                  {{.arity = 2, .spelling = "(NOT " + backend.Invoke("$1, $2") + ")"}});
    }
    definitions
        ->Scalar("bq_st_intersection", {kGeography, kGeography}, kGeography,
                 EachRow<SetOperation<S2BooleanOperation::OpType::INTERSECTION>>)
        .Call("ST_INTERSECTION", 2);
    definitions
        ->Scalar("bq_st_difference", {kGeography, kGeography}, kGeography,
                 EachRow<SetOperation<S2BooleanOperation::OpType::DIFFERENCE>>)
        .Call("ST_DIFFERENCE", 2);
    definitions
        ->Scalar("bq_st_boundary", {kGeography}, kGeography,
                 EachRow<Call<googlesql_geography::Boundary>>)
        .Call("ST_BOUNDARY", 1);
    definitions
        ->Scalar("bq_st_touches", {kGeography, kGeography}, kBoolean,
                 EachRow<Call<googlesql_geography::Touches>>)
        .Call("ST_TOUCHES", 2);
    {
      auto& backend = definitions
                          ->Scalar("bq_st_covers", {kGeography, kGeography}, kBoolean,
                                   EachRow<Call<googlesql_geography::Covers>>)
                          .Call("ST_COVERS", 2);
      backend.Sql("ST_COVEREDBY", {{.arity = 2, .spelling = backend.Invoke("$2, $1")}});
    }
    {
      auto& backend = definitions
                          ->Scalar("bq_st_contains", {kGeography, kGeography}, kBoolean,
                                   EachRow<Call<googlesql_geography::Contains>>)
                          .Call("ST_CONTAINS", 2);
      backend.Sql("ST_WITHIN", {{.arity = 2, .spelling = backend.Invoke("$2, $1")}});
    }
    definitions->Scalar("bq_st_x", {kGeography}, kDouble, EachRow<Call<googlesql_geography::X>>)
        .Call("ST_X", 1);
    definitions->Scalar("bq_st_y", {kGeography}, kDouble, EachRow<Call<googlesql_geography::Y>>)
        .Call("ST_Y", 1);
    definitions
        ->Scalar("bq_st_geometrytype", {kGeography}, kVarchar,
                 EachRow<OfGeography<googlesql_geography::GeometryType>>)
        .Call("ST_GEOMETRYTYPE", 1);
    definitions
        ->Scalar("bq_st_dimension", {kGeography}, kBigint,
                 EachRow<OfGeography<googlesql_geography::Dimension>>)
        .Call("ST_DIMENSION", 1);
    definitions
        ->Scalar("bq_st_numpoints", {kGeography}, kBigint,
                 EachRow<OfGeography<googlesql_geography::NumPoints>>)
        .Call("ST_NUMPOINTS", 1)
        .Call("ST_NPOINTS", 1);
    definitions
        ->Scalar("bq_st_isempty", {kGeography}, kBoolean,
                 EachRow<OfGeography<[](const Geography& geography) { return geography.Empty(); }>>)
        .Call("ST_ISEMPTY", 1);
    definitions
        ->Scalar(
            "bq_st_iscollection", {kGeography}, kBoolean,
            EachRow<OfGeography<[](const Geography& geography) { return geography.Count() > 1; }>>)
        .Call("ST_ISCOLLECTION", 1);
    definitions
        ->Scalar("bq_st_numgeometries", {kGeography}, kBigint,
                 EachRow<OfGeography<[](const Geography& geography) {
                   return static_cast<int64_t>(geography.Count());
                 }>>)
        .Call("ST_NUMGEOMETRIES", 1);
    definitions
        ->Scalar("bq_st_isclosed", {kGeography}, kBoolean,
                 EachRow<OfGeography<googlesql_geography::IsClosed>>)
        .Call("ST_ISCLOSED", 1);
    definitions
        ->Scalar("bq_st_isring", {kGeography}, kBoolean,
                 EachRow<OfGeography<googlesql_geography::IsRing>>)
        .Call("ST_ISRING", 1);
    definitions
        ->Scalar("bq_st_length", {kGeography, kBoolean, kVarchar}, kDouble,
                 EachRow<Call<googlesql_geography::Length>>)
        .Spheroid("ST_LENGTH", {1, 2});
    definitions
        ->Scalar("bq_st_measure", {kGeography, kBoolean, kVarchar}, kDouble,
                 EachRow<Call<googlesql_geography::Measure>>)
        .Spheroid("ST_AREA", {1, 2})
        .Spheroid("ST_PERIMETER", {1, 2});
    definitions
        ->Scalar("bq_st_exteriorring", {kGeography}, kGeography,
                 EachRow<Call<googlesql_geography::ExteriorRing>>)
        .Call("ST_EXTERIORRING", 1);
    definitions
        ->Scalar("bq_st_makepolygon", {kGeography}, kGeography,
                 EachRow<[](const Arguments& args) { return MakePolygon({args.String(0)}); }>)
        .Call("ST_MAKEPOLYGON", 1);
    {
      auto& backend = definitions->Scalar("bq_st_pointn", {kGeography, kBigint}, kGeography,
                                          EachRow<Call<googlesql_geography::PointN>>);
      backend.Sql("ST_STARTPOINT", {{.arity = 1, .spelling = backend.Invoke("$1, 1")}});
      backend.Sql("ST_ENDPOINT", {{.arity = 1, .spelling = backend.Invoke("$1, -1")}});
      backend.Sql("ST_POINTN", {{.arity = 2, .spelling = backend.Invoke("$1, $2")}});
    }
    definitions
        ->Scalar("bq_st_makeline", {kGeography, kGeography}, kGeography,
                 EachRow<[](const Arguments& args) {
                   return MakeLine({args.String(0), args.String(1)});
                 }>)
        .Call("ST_MAKELINE", 2);
    definitions
        ->Scalar("bq_st_union", {kGeography, kGeography}, kGeography,
                 EachRow<[](const Arguments& args) {
                   return UnionGeographies({args.String(0), args.String(1)});
                 }>)
        .Call("ST_UNION", 2);
    {
      auto& backend = definitions->Custom(kUnionArray, [](duckdb_connection connection,
                                                          const char* name,
                                                          const Options& /*options*/) {
        const LogicalType geography(duckdb_create_logical_type(kGeography));
        const LogicalType list(duckdb_create_list_type(geography.get()));
        Register(connection, name, {list.get()}, geography.get(),
                 EachRow<[](const Arguments& args) { return UnionGeographies(args.Strings(0)); }>);
      });
      backend.Sql("ST_UNION",
                  {
                      {
                          .arity = 1,
                          .spelling = backend.Invoke(
                              "list_filter($1, lambda geography: geography IS NOT NULL)"),
                      },
                  });
    }
    {
      auto& backend = definitions->Custom(
          "bq_st_makeline_array",
          [](duckdb_connection connection, const char* name, const Options& /*options*/) {
            const LogicalType geography(duckdb_create_logical_type(kGeography));
            const LogicalType list(duckdb_create_list_type(geography.get()));
            Register(connection, name, {list.get()}, geography.get(),
                     EachRow<[](const Arguments& args) { return MakeLine(args.Strings(0)); }>);
          });
      backend.Sql("ST_MAKELINE",
                  {
                      {
                          .arity = 1,
                          .spelling = "CASE WHEN list_count($1) <> len($1) THEN NULL ELSE " +
                                      backend.Invoke("$1") + " END",
                      },
                  });
    }
    {
      auto& backend = definitions->Custom(
          "bq_st_makepolygon_holes",
          [](duckdb_connection connection, const char* name, const Options& /*options*/) {
            const LogicalType geography(duckdb_create_logical_type(kGeography));
            const LogicalType list(duckdb_create_list_type(geography.get()));
            Register(connection, name,
                     std::vector<duckdb_logical_type>{geography.get(), list.get()}, geography.get(),
                     EachRow<[](const Arguments& args) {
                       std::vector<std::string> rings = args.Strings(1);
                       rings.insert(rings.begin(), args.String(0));
                       return MakePolygon(rings);
                     }>);
          });
      backend.Sql("ST_MAKEPOLYGON",
                  {
                      {
                          .arity = 2,
                          .spelling = "CASE WHEN list_count($2) <> len($2) THEN NULL ELSE " +
                                      backend.Invoke("$1, $2") + " END",
                      },
                  });
    }
    {
      auto& backend = definitions->Custom("bq_st_dump", [](duckdb_connection connection,
                                                           const char* name,
                                                           const Options& /*options*/) {
        const LogicalType geography(duckdb_create_logical_type(kGeography));
        const LogicalType list(duckdb_create_list_type(geography.get()));
        const LogicalType bigint(duckdb_create_logical_type(kBigint));
        Register(connection, name, std::vector<duckdb_logical_type>{geography.get(), bigint.get()},
                 list.get(), EachRow<Call<googlesql_geography::Dump>>);
      });
      backend.Sql("ST_DUMP",
                  {
                      {.arity = 1, .spelling = backend.Invoke("$1, -1")},
                      {
                          .arity = 2,
                          .spelling = "CASE WHEN $2 IS NULL THEN CASE WHEN $1 IS NULL THEN "
                                      "NULL ELSE []::BLOB[] END ELSE " +
                                      backend.Invoke("$1, $2") + " END",
                      },
                  });
    }
    definitions
        ->Custom("bq_st_interiorrings",
                 [](duckdb_connection connection, const char* name, const Options& /*options*/) {
                   const LogicalType geography(duckdb_create_logical_type(kGeography));
                   const LogicalType list(duckdb_create_list_type(geography.get()));
                   Register(connection, name, {geography.get()}, list.get(),
                            EachRow<Call<googlesql_geography::InteriorRings>>);
                 })
        .Call("ST_INTERIORRINGS", 1);
    definitions
        ->Scalar("bq_st_geohash", {kGeography, kBigint}, kVarchar,
                 EachRow<Call<googlesql_geography::Geohash>>)
        .Call("ST_GEOHASH", {1, 2}, {"20"}, {TypeUse::kRejectsSafe});
    definitions
        ->Scalar("bq_s2_cellidfrompoint", {kGeography, kBigint}, kBigint,
                 EachRow<Call<googlesql_geography::CellIdFromPoint>>)
        .Call("S2_CELLIDFROMPOINT", {1, 2}, {"30"});
    definitions->Custom("bq_st_boundingbox", RegisterBounds<false>).Call("ST_BOUNDINGBOX", 1);
    // The aggregates' finishes, over the non-NULL values that list() collects.
    definitions->Custom(kCentroidAggregate, [](duckdb_connection connection, const char* name,
                                               const Options& /*options*/) {
      const LogicalType geography(duckdb_create_logical_type(kGeography));
      const LogicalType list(duckdb_create_list_type(geography.get()));
      Register(connection, name, {list.get()}, geography.get(),
               EachRow<[](const Arguments& args) { return CentroidAggregate(args.Strings(0)); }>);
    });
    definitions->Custom(kExtent, RegisterBounds<true>);
    return definitions;
  }();
  return *kDefinitions;
}

}  // namespace

const std::unordered_map<std::string_view, FunctionDefinition<Rule>>& GeographyFunctions() {
  static const auto* const kFunctions = [] {
    auto* functions = new std::unordered_map<std::string_view, FunctionDefinition<Rule>>();
    for (const BackendDefinition& backend : GeographyDefinitions().backends) {
      for (const auto& [name, definition] : backend.functions) {
        auto& function = (*functions)[name];
        function.rules.insert(function.rules.end(), definition.rules.begin(),
                              definition.rules.end());
        function.uses |= definition.uses;
      }
    }
    return functions;
  }();
  return *kFunctions;
}

namespace {

// An aggregate of the non-NULL values that list() collects, which the registered `kFunction`
// finishes. ST_UNION_AGG unions them as ST_UNION does an array, which puts all the points first,
// so none of these folds the values one at a time.
template <const char* kFunction>
FunctionDefinition<AggregateRule> ListAggregate() {
  return {
      .rules =
          {
              {
                  .function = "list",
                  .distinct = AggregateRule::Distinct::kUnsupported,
                  .skip_nulls = true,
                  .finish =
                      [](const std::string& sql, const std::vector<std::string>& /*arguments*/,
                         const std::string& /*tail*/) {
                        return std::string(kFunction) + "(" + sql + ")";
                      },
              },
          },
      // BigQuery takes none of them over a window.
      .uses = {TypeUse::kBuildsOrReadsGeography, TypeUse::kRejectsOver},
  };
}

}  // namespace

const std::unordered_map<std::string_view, FunctionDefinition<AggregateRule>>&
GeographyAggregates() {
  static const auto* const kAggregates =
      new std::unordered_map<std::string_view, FunctionDefinition<AggregateRule>>{
          {"ST_UNION_AGG", ListAggregate<kUnionArray>()},
          {"ST_CENTROID_AGG", ListAggregate<kCentroidAggregate>()},
          {"ST_EXTENT", ListAggregate<kExtent>()},
  };
  return *kAggregates;
}

void RegisterGeographyFunctions(duckdb_connection connection,
                                const googlesql_geography::Options& options) {
  RegisterGeographyStorage(connection);
  for (const BackendDefinition& backend : GeographyDefinitions().backends) {
    backend.install(connection, backend.name, options);
  }
}

}  // namespace bigquery_emulator_duckdb::geography_duckdb
