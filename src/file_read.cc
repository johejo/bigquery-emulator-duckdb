#include "src/file_read.h"

#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "googlesql/public/functions/convert_string.h"
#include "googlesql/public/numeric_value.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/bignumeric.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/file_read_options.h"
#include "src/gcs.h"
#include "src/parquet_metadata.h"
#include "src/schema_sql.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

std::vector<FileSource> StageFileSources(const std::vector<std::string>& uris, FileFormat format,
                                         GcsClient& gcs, TemporaryFiles& downloads) {
  std::vector<std::string> sources;
  for (const std::string& uri : uris) {
    if (uri.starts_with("gs://")) {
      const auto matches = gcs.Expand(uri);
      sources.insert(sources.end(), matches.begin(), matches.end());
    } else {
      sources.push_back(uri);
    }
  }
  std::vector<FileSource> staged;
  for (const std::string& uri : sources) {
    std::string path;
    if (uri.starts_with("gs://")) {
      path = downloads.Create();
      gcs.Download(uri, std::filesystem::path(path));
    } else if (uri.starts_with("file://")) {
      path = uri.substr(7);
    } else if (uri.find("://") == std::string::npos) {
      path = uri;
    } else {
      throw ApiError::Invalid("Unsupported source URI: " + uri);
    }
    if (path.empty() || !std::filesystem::is_regular_file(path)) {
      throw ApiError::Invalid("Source file does not exist: " + uri);
    }
    // Uploads and downloads lose their original suffix. DuckDB selects gzip by suffix,
    // so inspect the bytes and stage gzip inputs under a name its readers recognize.
    if (format != FileFormat::kParquet) {
      std::ifstream input(path, std::ios::binary);
      const bool gzip = input.get() == 0x1f && input.get() == 0x8b;
      if (gzip) {
        const std::string compressed = downloads.Create(".gz");
        if (uri.starts_with("gs://")) {
          std::filesystem::rename(path, compressed);
        } else {
          std::filesystem::copy_file(path, compressed,
                                     std::filesystem::copy_options::overwrite_existing);
        }
        path = compressed;
      }
    }
    staged.push_back({.uri = uri, .path = path});
  }
  return staged;
}

namespace {

constexpr int64_t kBigNumericScale = googlesql::BigNumericValue::kMaxFractionalDigits;

bool HasType(const std::vector<FieldSchema>& fields, FieldType type) {
  return std::ranges::any_of(fields, [type](const FieldSchema& field) {
    return field.type == type || HasType(field.fields, type);
  });
}

// Rewrites a JSON value, writing the numbers of the NUMERIC and BIGNUMERIC fields of a schema as
// strings that DuckDB casts exactly: a NUMERIC as its text, and a BIGNUMERIC as its units (see
// src/bignumeric.h), which a CAST to BIGNUM at any depth reads. The fields of objects are matched
// by name in any case. It implements json::sax_parse's interface.
class NumericQuoter {
 public:
  explicit NumericQuoter(const std::vector<FieldSchema>& schema) : schema_(schema) {}

  [[nodiscard]] const std::string& Output() const { return out_; }

  bool null() {
    Separate();
    out_ += "null";
    return true;
  }
  bool boolean(bool value) {
    Separate();
    out_ += value ? "true" : "false";
    return true;
  }
  bool number_integer(json::number_integer_t value) { return Number(std::to_string(value)); }
  bool number_unsigned(json::number_unsigned_t value) { return Number(std::to_string(value)); }
  bool number_float(json::number_float_t /*value*/, const json::string_t& text) {
    return Number(text);
  }
  bool string(json::string_t& value) {
    Separate();
    if (IsField(FieldType::kBigNumeric)) {
      WriteBigNumeric(value);
    } else {
      out_ += json(value).dump();
    }
    return true;
  }
  // JSON text has no binary values.
  static bool binary(json::binary_t& /*value*/) { return false; }
  bool start_object(std::size_t /*elements*/) {
    Separate();
    const FieldSchema* field = ValueField();
    const std::vector<FieldSchema>* fields = frames_.empty() ? &schema_
                                             : field != nullptr && field->type == FieldType::kRecord
                                                 ? &field->fields
                                                 : nullptr;
    frames_.push_back({.fields = fields});
    out_ += '{';
    return true;
  }
  bool key(json::string_t& name) {
    Frame& frame = frames_.back();
    if (!frame.first) {
      out_ += ',';
    }
    frame.first = false;
    out_ += json(name).dump() + ':';
    frame.field = nullptr;
    if (frame.fields != nullptr) {
      const auto found = std::ranges::find_if(*frame.fields, [&name](const FieldSchema& field) {
        return absl::EqualsIgnoreCase(field.name, name);
      });
      if (found != frame.fields->end()) {
        frame.field = &*found;
      }
    }
    return true;
  }
  bool end_object() {
    frames_.pop_back();
    out_ += '}';
    return true;
  }
  bool start_array(std::size_t /*elements*/) {
    Separate();
    // The elements of a REPEATED field have its type.
    frames_.push_back({.field = ValueField(), .array = true});
    out_ += '[';
    return true;
  }
  bool end_array() {
    frames_.pop_back();
    out_ += ']';
    return true;
  }
  static bool parse_error(std::size_t /*position*/, const std::string& /*token*/,
                          const nlohmann::detail::exception& error) {
    throw ApiError::Invalid(std::string("Invalid JSON: ") + error.what());
  }

 private:
  struct Frame {
    // An object's fields, or null when the schema does not describe it.
    const std::vector<FieldSchema>* fields = nullptr;
    // The field of an array's elements, or of the value after an object's last key.
    const FieldSchema* field = nullptr;
    bool array = false;
    bool first = true;
  };

  [[nodiscard]] const FieldSchema* ValueField() const {
    return frames_.empty() ? nullptr : frames_.back().field;
  }

  [[nodiscard]] bool IsField(FieldType type) const {
    const FieldSchema* field = ValueField();
    return field != nullptr && field->type == type;
  }

  // Writes the comma before an array's elements after the first; key() writes an object's.
  void Separate() {
    if (frames_.empty() || !frames_.back().array) {
      return;
    }
    if (!frames_.back().first) {
      out_ += ',';
    }
    frames_.back().first = false;
  }

  bool Number(const std::string& text) {
    Separate();
    if (IsField(FieldType::kBigNumeric)) {
      WriteBigNumeric(text);
    } else if (IsField(FieldType::kNumeric)) {
      out_ += json(text).dump();
    } else {
      out_ += text;
    }
    return true;
  }

  void WriteBigNumeric(std::string_view text) {
    googlesql::BigNumericValue value;
    absl::Status error;
    if (!googlesql::functions::StringToNumeric(text, &value, &error)) {
      throw ApiError::Invalid(std::string(error.message()));
    }
    out_ += '"' + BigNumericUnits(value) + '"';
  }

  const std::vector<FieldSchema>& schema_;
  std::vector<Frame> frames_;
  std::string out_;
};

// `field` with each GEOGRAPHY in it, at any depth, a STRING: a load reads a GEOGRAPHY as its
// text, which StoredValue converts.
FieldSchema TextGeographies(FieldSchema field) {
  if (field.type == FieldType::kGeography) {
    field.type = FieldType::kString;
  }
  for (FieldSchema& child : field.fields) {
    child = TextGeographies(std::move(child));
  }
  return field;
}

// The DuckDB value `sql` of `column`, a Parquet column or null for none, as `field`. DuckDB casts
// most columns itself, but StageParquetSources makes it read a wide DECIMAL as its bytes, and
// it casts a DECIMAL to BIGNUM as an integer, so those go through GoogleSQL's conversions. A
// lambda takes the name _e and its `depth`.
std::string ParquetValue(const std::string& sql, const FieldSchema& field,
                         const ParquetColumn* column, int depth) {
  std::string cast = std::format("CAST({} AS {})", sql, ToDuckDbType(field));
  if (column == nullptr ||
      (field.type != FieldType::kGeography && !HasType(field.fields, FieldType::kGeography) &&
       field.type != FieldType::kBigNumeric && !HasType(field.fields, FieldType::kBigNumeric) &&
       !column->HasWideDecimal())) {
    return cast;
  }
  // Where the shapes differ, DuckDB's cast reports it.
  if (field.mode == FieldMode::kRepeated) {
    if (column->kind != ParquetColumn::Kind::kList) {
      return cast;
    }
    return MapElements(sql, field, depth,
                       [&](const std::string& name, const FieldSchema& element, int element_depth) {
                         return ParquetValue(name, element, &column->children.front(),
                                             element_depth);
                       });
  }
  if (field.type == FieldType::kRecord) {
    if (column->kind != ParquetColumn::Kind::kStruct) {
      return cast;
    }
    return MapFields(sql, field, [&](const std::string& value, const FieldSchema& child) {
      return ParquetValue(value, child, column->Child(child.name), depth);
    });
  }
  if (field.type == FieldType::kGeography) {
    if ((column->type == parquet::kByteArray || column->type == parquet::kFixedLenByteArray) &&
        column->converted == -1 && column->logical == 0) {
      return std::format("bq_parquet_wkb_input({}, {})", sql,
                         column->edges == ParquetColumn::Edges::kPlanar ? "true" : "false");
    }
    return std::format("bq_geography_input(CAST({} AS VARCHAR))", sql);
  }
  if (column->IsWideDecimal() &&
      (field.type != FieldType::kBigNumeric || column->scale > kBigNumericScale)) {
    throw ApiError::Unsupported(
        std::format("loading a Parquet DECIMAL({}, {}) column into {} field {}", column->precision,
                    column->scale, FieldTypeName(field.type), field.name));
  }
  if (field.type != FieldType::kBigNumeric) {
    return cast;
  }
  if (column->IsWideDecimal()) {
    return std::format("CAST(bq_bignumeric_from_decimal_bytes({}, {}) AS BIGNUM)", sql,
                       column->scale);
  }
  if (column->IsDecimal() || column->IsInteger()) {
    return std::format("CAST(bq_bignumeric_from_string(CAST({} AS VARCHAR), false) AS BIGNUM)",
                       sql);
  }
  throw ApiError::Unsupported(std::format(
      "loading BIGNUMERIC field {} from a Parquet column that is not a DECIMAL or an integer",
      field.name));
}

// The DuckDB value `sql` of `column` in the shape BigQuery loads it, with each unwrapped STRUCT
// put back around the field DuckDB reads in its place, a NULL list empty and a null element of a
// list an error. A lambda takes the name _p and its `depth`. An `element` of a list is a repeated
// group, which is never NULL.
std::string ParquetShape(const std::string& sql, const ParquetColumn& column, int depth,
                         bool element) {
  if (!column.Reshaped()) {
    return sql;
  }
  if (column.kind == ParquetColumn::Kind::kList) {
    const std::string name = "_p" + std::to_string(depth);
    std::string value = ParquetShape(name, column.children.front(), depth + 1, true);
    if (column.null_elements) {
      value = std::format(
          "CASE WHEN {} IS NULL THEN error({}) ELSE {} END", name,
          QuoteLiteral("Array cannot have a null element; error in writing field " + column.name),
          value);
    }
    std::string list =
        value == name ? sql : std::format("list_transform({}, lambda {}: {})", sql, name, value);
    return column.nullable ? std::format("coalesce({}, [])", list) : list;
  }
  if (column.kind != ParquetColumn::Kind::kStruct) {
    throw ApiError::Unsupported(
        "loading a Parquet MAP with a STRUCT of one field that DuckDB reads as the field");
  }
  std::vector<std::string> fields;
  for (const ParquetColumn& child : column.children) {
    const std::string value =
        column.unwrapped ? sql
                         : std::format("struct_extract({}, {})", sql, QuoteLiteral(child.name));
    fields.push_back(std::format("{} := {}", QuoteIdentifier(child.name),
                                 ParquetShape(value, child, depth, false)));
  }
  std::string packed = std::format("struct_pack({})", Join(fields, ", "));
  if (element && column.unwrapped) {
    return packed;
  }
  return std::format("CASE WHEN {} IS NULL THEN NULL ELSE {} END", sql, packed);
}

// The schema elements of the wide DECIMAL leaves of `column`.
void WideDecimals(const ParquetColumn& column, std::vector<size_t>& elements) {
  if (column.IsWideDecimal()) {
    elements.push_back(column.element);
  }
  for (const ParquetColumn& child : column.children) {
    WideDecimals(child, elements);
  }
}

// Gives `field` the type BigQuery detects for `column` when it is a wide DECIMAL. Of the
// `targets`, decimalTargetTypes, NUMERIC cannot hold one, and BIGNUMERIC is picked unless it
// cannot either and STRING is listed. Converting to NUMERIC, which is picked when it is the only
// one, or to STRING is unsupported.
void DetectDecimals(FieldSchema& field, const ParquetColumn* column,
                    const std::vector<DecimalTargetType>& targets) {
  if (column == nullptr) {
    return;
  }
  if (field.mode == FieldMode::kRepeated) {
    if (column->kind != ParquetColumn::Kind::kList) {
      return;
    }
    column = &column->children.front();
  }
  if (field.type == FieldType::kRecord) {
    for (FieldSchema& child : field.fields) {
      DetectDecimals(child, column->Child(child.name), targets);
    }
    return;
  }
  if (!column->IsWideDecimal()) {
    return;
  }
  const auto listed = [&targets](DecimalTargetType type) {
    return std::ranges::find(targets, type) != targets.end();
  };
  const bool fits = column->precision <= 76 && column->scale <= kBigNumericScale;
  if (!listed(DecimalTargetType::kBigNumeric) || (!fits && listed(DecimalTargetType::kString))) {
    throw ApiError::Unsupported(std::format(
        "detecting the type of a Parquet DECIMAL({}, {}) column unless decimalTargetTypes picks "
        "BIGNUMERIC",
        column->precision, column->scale));
  }
  field.type = FieldType::kBigNumeric;
}

// `field` with each BIGNUMERIC or Base64-encoded BYTES in it, at any depth, a STRING.
// DuckDB's JSON reader cannot read a BIGNUM, which it would for a cast of a column it detects
// as JSON when every value is null. The reader reads those values as text, as StageJsonNumerics
// writes them.
FieldSchema JsonTextFields(FieldSchema field) {
  if (field.type == FieldType::kBigNumeric) {
    field.type = FieldType::kString;
    field.precision.reset();
    field.scale.reset();
    field.rounding_mode = RoundingMode::kUnspecified;
  }
  if (field.type == FieldType::kBytes) {
    field.type = FieldType::kString;
  }
  for (FieldSchema& child : field.fields) {
    child = JsonTextFields(std::move(child));
  }
  return field;
}

// The value of `field` that the column `name` of a NEWLINE_DELIMITED_JSON file holds.
std::string JsonValue(const std::string& name, const FieldSchema& field, int depth = 0) {
  if (HasType({field}, FieldType::kBytes)) {
    const std::string text = "CAST(" + name + " AS " + ToDuckDbType(JsonTextFields(field)) + ")";
    if (field.mode == FieldMode::kRepeated) {
      return MapElements(text, field, depth, JsonValue);
    }
    if (field.type == FieldType::kRecord) {
      return MapFields(text, field, [depth](const std::string& value, const FieldSchema& child) {
        return JsonValue(value, child, depth);
      });
    }
    return "from_base64(CAST(" + name + " AS VARCHAR))";
  }
  const FieldSchema read = JsonTextFields(field);
  const std::string type = ToDuckDbType(field);
  if (read.type == field.type && !HasType(field.fields, FieldType::kBigNumeric)) {
    return std::format("CAST({} AS {})", name, type);
  }
  return std::format("CAST(CAST({} AS {}) AS {})", name, ToDuckDbType(read), type);
}

}  // namespace

std::string ReadMaybeGzip(const std::string& path) {
  gzFile input = gzopen(path.c_str(), "rb");
  if (input == nullptr) {
    throw ApiError::Invalid("Could not read " + path);
  }
  std::string contents;
  std::string buffer(1U << 16U, '\0');
  int read = 0;
  while ((read = gzread(input, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0) {
    contents.append(buffer, 0, static_cast<std::size_t>(read));
  }
  const bool ok = read == 0;
  gzclose(input);
  if (!ok) {
    throw ApiError::Invalid("Could not read " + path);
  }
  return contents;
}

ParquetSources StageParquetSources(const std::vector<FileSource>& inputs, TemporaryFiles& downloads,
                                   bool list_inference) {
  ParquetSources sources;
  for (const FileSource& input : inputs) {
    const std::string& path = input.path;
    ThriftValue metadata = ReadParquetMetadata(path);
    ParquetColumn columns = ParquetColumns(metadata, list_inference);
    bool geo = false;
    // FileMetaData.key_value_metadata (5), KeyValue.key (1), value (2).
    // Hide geo from DuckDB: its native GEOMETRY reader discards the edge interpretation.
    if (ThriftValue* entries = metadata.Field(5)) {
      std::erase_if(entries->elements, [&](const ThriftValue& entry) {
        const ThriftValue* key = entry.Field(1);
        if (key == nullptr || key->bytes != "geo") {
          return false;
        }
        geo = true;
        const ThriftValue* value = entry.Field(2);
        if (value == nullptr) {
          throw ApiError::Invalid("GeoParquet metadata has no value");
        }
        json description;
        try {
          description = json::parse(value->bytes);
          for (const auto& [name, definition] : description.at("columns").items()) {
            const auto found = std::ranges::find(columns.children, name, &ParquetColumn::name);
            if (found == columns.children.end() || found->kind != ParquetColumn::Kind::kLeaf ||
                found->type != parquet::kByteArray) {
              throw ApiError::Unsupported("GeoParquet geometry column that is not binary WKB");
            }
            if (definition.at("encoding") != "WKB") {
              throw ApiError::Unsupported("GeoParquet encoding other than WKB");
            }
            if (definition.contains("crs") && (!definition["crs"].is_object() ||
                                               definition["crs"].value("id", json::object()) !=
                                                   json{{"authority", "OGC"}, {"code", "CRS84"}})) {
              throw ApiError::Unsupported("GeoParquet CRS other than CRS84");
            }
            if (definition.contains("epoch")) {
              throw ApiError::Unsupported("GeoParquet coordinate epoch");
            }
            const std::string edges = definition.value("edges", "planar");
            if (edges != "planar" && edges != "spherical") {
              throw ApiError::Unsupported("GeoParquet edge interpretation " + edges);
            }
            found->edges = edges == "planar" ? ParquetColumn::Edges::kPlanar
                                             : ParquetColumn::Edges::kSpherical;
            const auto& element = metadata.Field(parquet::kSchema)->elements.at(found->element);
            const auto* repetition = element.Field(parquet::kElementRepetition);
            found->required = repetition != nullptr && repetition->integer == 0;
          }
        } catch (const json::exception& error) {
          throw ApiError::Invalid(std::string("Invalid GeoParquet metadata: ") + error.what());
        }
        return true;
      });
    }
    if (sources.sources.empty()) {
      sources.columns = columns;
    } else if (columns != sources.columns &&
               (columns.HasWideDecimal() || sources.columns.HasWideDecimal())) {
      throw ApiError::Unsupported(
          "loading Parquet files whose schemas differ when one has a DECIMAL "
          "wider than 38 digits");
    } else if (columns != sources.columns &&
               (geo || std::ranges::any_of(sources.columns.children, [](const auto& column) {
                  return column.edges != ParquetColumn::Edges::kNone;
                }))) {
      throw ApiError::Unsupported("loading Parquet files with differing GeoParquet metadata");
    } else if (columns != sources.columns && (columns.Reshaped() || sources.columns.Reshaped())) {
      throw ApiError::Unsupported(
          "loading Parquet files whose schemas differ when one has a STRUCT of one field that "
          "DuckDB reads as the field or a nullable LIST");
    }
    std::vector<size_t> wide;
    WideDecimals(columns, wide);
    if (wide.empty() && !geo) {
      sources.sources.push_back(input);
      continue;
    }
    for (const size_t index : wide) {
      ThriftValue& element = metadata.Field(parquet::kSchema)->elements.at(index);
      for (const int16_t id : {
               parquet::kElementConvertedType,
               parquet::kElementScale,
               parquet::kElementPrecision,
               parquet::kElementLogicalType,
           }) {
        element.RemoveField(id);
      }
    }
    const std::string staged = downloads.Create();
    std::filesystem::copy_file(path, staged, std::filesystem::copy_options::overwrite_existing);
    RewriteParquetMetadata(staged, metadata);
    sources.sources.push_back({.uri = input.uri, .path = staged});
  }
  return sources;
}

void DetectParquetSchema(std::vector<FieldSchema>& schema, const ParquetColumn& columns,
                         const std::vector<DecimalTargetType>& targets) {
  for (FieldSchema& field : schema) {
    DetectDecimals(field, columns.Child(field.name), targets);
    const ParquetColumn* column = columns.Child(field.name);
    if (column != nullptr && column->edges != ParquetColumn::Edges::kNone) {
      field.type = FieldType::kGeography;
      field.mode = column->required ? FieldMode::kRequired : FieldMode::kNullable;
    }
  }
}

std::vector<FileSource> StageJsonNumerics(const std::vector<FileSource>& sources,
                                          const std::vector<FieldSchema>& schema,
                                          TemporaryFiles& downloads) {
  if (!HasType(schema, FieldType::kNumeric) && !HasType(schema, FieldType::kBigNumeric)) {
    return sources;
  }
  std::vector<FileSource> staged;
  for (const FileSource& source : sources) {
    const std::string contents = ReadMaybeGzip(source.path);
    std::string rewritten;
    for (std::size_t start = 0; start < contents.size();) {
      std::size_t end = contents.find('\n', start);
      if (end == std::string::npos) {
        end = contents.size();
      }
      const std::string_view line(contents.data() + start, end - start);
      if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
        rewritten += line;
      } else {
        NumericQuoter quoter(schema);
        json::sax_parse(line, &quoter);
        rewritten += quoter.Output();
      }
      rewritten += '\n';
      start = end + 1;
    }
    staged.push_back({.uri = source.uri, .path = downloads.Write(rewritten)});
  }
  return staged;
}

std::string FileReadQuery(const FileReadOptions& options, const std::vector<FileSource>& sources,
                          const std::vector<FieldSchema>& schema, const ParquetColumn& parquet) {
  const auto* csv = std::get_if<CsvReadOptions>(&options);
  const bool is_parquet = std::holds_alternative<ParquetReadOptions>(options);
  // BigQuery fails on a Parquet column that is not a group, but loads a group of `start` and
  // `end` as NULL, which the emulator rejects as unsupported below.
  if (is_parquet) {
    for (const FieldSchema& field : schema) {
      const ParquetColumn* column = parquet.Child(field.name);
      if (column != nullptr && column->edges != ParquetColumn::Edges::kNone &&
          field.type != FieldType::kGeography) {
        throw ApiError::Unsupported("loading a GeoParquet geometry into a non-GEOGRAPHY field");
      }
      if (field.type == FieldType::kRange && column != nullptr &&
          column->kind == ParquetColumn::Kind::kLeaf) {
        throw ApiError::Invalid(
            std::format("Error while reading data, error message: Attempted to read column '{}' "
                        "as a BigQuery RANGE, which is not currently supported.",
                        column->name));
      }
    }
  }
  for (const FieldType type : {FieldType::kInterval, FieldType::kRange}) {
    if (csv == nullptr && HasType(schema, type)) {
      throw ApiError::Unsupported("loading " + std::string(FieldTypeName(type)) + " columns from " +
                                  std::string(is_parquet ? "PARQUET" : "NEWLINE_DELIMITED_JSON"));
    }
  }
  std::vector<std::string> quoted;
  std::ranges::transform(sources, std::back_inserter(quoted),
                         [](const FileSource& source) { return QuoteLiteral(source.path); });
  const std::string files = "[" + Join(quoted, ", ") + "]";
  std::string sql;
  if (csv != nullptr) {
    sql = "SELECT * FROM read_csv(" + files +
          ", header=false, skip=" + std::to_string(csv->skip_leading_rows) +
          ", delim=" + QuoteLiteral(csv->field_delimiter);
    if (!csv->quoted_nulls) {
      sql += ", allow_quoted_nulls=false";
    }
    if (!schema.empty()) {
      sql += ", auto_detect=false";
      std::vector<std::string> columns;
      // read_csv() would truncate a BIGNUM, so a BIGNUMERIC is read as text and converted, as
      // are a RANGE, which has no text form in DuckDB, and an INTERVAL, whose text form differs.
      // A GEOGRAPHY is read from its text, as in the other formats.
      std::vector<std::string> conversions;
      for (const FieldSchema& field : schema) {
        const bool big = field.type == FieldType::kBigNumeric;
        const bool range = field.type == FieldType::kRange;
        const bool interval = field.type == FieldType::kInterval;
        const bool bytes = field.type == FieldType::kBytes;
        const std::string name = QuoteIdentifier(field.name);
        columns.push_back(QuoteLiteral(field.name) + ": " +
                          QuoteLiteral(big || range || interval || bytes
                                           ? "VARCHAR"
                                           : ToDuckDbType(TextGeographies(field))));
        conversions.push_back(
            big     ? std::format("CAST(bq_bignumeric_from_string({0}, false) AS {1}) AS {0}", name,
                                  ToDuckDbType(field))
            : range ? RangeFromText(name, field) + " AS " + name
            : interval
                ? std::format("bq_interval_from_csv({0}) AS {0}", name)
                : std::format(
                      "{} AS {}",
                      StoredValue(bytes ? std::format("from_base64({})", name) : name, field),
                      name));
      }
      sql += ", columns={" + Join(columns, ", ") + "})";
      return std::format("SELECT {} FROM ({}) AS source", Join(conversions, ", "), sql);
    }
    sql += ')';
  } else if (std::holds_alternative<JsonReadOptions>(options)) {
    sql = std::format("SELECT * FROM read_json({}, format='newline_delimited')", files);
    // A column no row has is NULL: UNION ALL BY NAME, which matches names without case, adds it.
    if (!schema.empty()) {
      std::vector<std::string> nulls;
      std::ranges::transform(schema, std::back_inserter(nulls), [](const FieldSchema& field) {
        return "NULL AS " + QuoteIdentifier(field.name);
      });
      sql = std::format("SELECT {} WHERE false UNION ALL BY NAME {}", Join(nulls, ", "), sql);
    }
  } else {
    sql = std::format("SELECT * FROM read_parquet({})", files);
    std::vector<std::string> shaped;
    for (const ParquetColumn& column : parquet.children) {
      if (column.Reshaped()) {
        const std::string name = QuoteIdentifier(column.name);
        shaped.push_back(std::format("{} AS {}", ParquetShape(name, column, 0, false), name));
      }
    }
    if (!shaped.empty()) {
      sql = std::format("SELECT * REPLACE ({}) FROM read_parquet({})", Join(shaped, ", "), files);
    }
  }
  if (!schema.empty() && csv == nullptr) {
    std::vector<std::string> columns;
    for (const FieldSchema& field : schema) {
      const std::string name = QuoteIdentifier(field.name);
      const std::string value =
          is_parquet
              ? CheckedWriteValue(ParquetValue(name, field, parquet.Child(field.name), 0), field)
              : StoredValue(JsonValue(name, TextGeographies(field)), field);
      columns.push_back(std::format("{} AS {}", value, name));
    }
    sql = std::format("SELECT {} FROM ({}) AS source", Join(columns, ", "), sql);
  }
  return sql;
}

}  // namespace bigquery_emulator_duckdb
