#include "src/load.h"

#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
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
#include "src/gcs.h"
#include "src/parquet_metadata.h"
#include "src/schema_sql.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

std::vector<std::string> StageLoadSources(const json& config, const std::string& format,
                                          GcsClient& gcs, TemporaryFiles& downloads) {
  const json uris = config.value("sourceUris", json::array());
  if (!uris.is_array() || uris.empty()) throw ApiError::Invalid("sourceUris is required");
  std::vector<std::string> sources;
  for (const json& item : uris) {
    if (!item.is_string()) throw ApiError::Invalid("Invalid source URI");
    const std::string uri = item.get<std::string>();
    if (uri.starts_with("gs://")) {
      const auto matches = gcs.Expand(uri);
      sources.insert(sources.end(), matches.begin(), matches.end());
    } else {
      sources.push_back(uri);
    }
  }
  std::vector<std::string> paths;
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
    if (format != "PARQUET") {
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
    paths.push_back(path);
  }
  return paths;
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
    if (!frame.first) out_ += ',';
    frame.first = false;
    out_ += json(name).dump() + ':';
    frame.field = nullptr;
    if (frame.fields != nullptr) {
      const auto found = std::ranges::find_if(*frame.fields, [&name](const FieldSchema& field) {
        return absl::EqualsIgnoreCase(field.name, name);
      });
      if (found != frame.fields->end()) frame.field = &*found;
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
    if (frames_.empty() || !frames_.back().array) return;
    if (!frames_.back().first) out_ += ',';
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

// The contents of the file at `path`, decompressed when it is gzip.
std::string ReadMaybeGzip(const std::string& path) {
  gzFile input = gzopen(path.c_str(), "rb");
  if (input == nullptr) throw ApiError::Invalid("Could not read " + path);
  std::string contents;
  std::string buffer(1U << 16U, '\0');
  int read = 0;
  while ((read = gzread(input, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0) {
    contents.append(buffer, 0, static_cast<std::size_t>(read));
  }
  const bool ok = read == 0;
  gzclose(input);
  if (!ok) throw ApiError::Invalid("Could not read " + path);
  return contents;
}

constexpr std::string_view kUnsupported = "The emulator does not support ";

// The DuckDB value `sql` of `column`, a Parquet column or null for none, as `field`. DuckDB casts
// most columns itself, but StageParquetDecimals makes it read a wide DECIMAL as its bytes, and
// it casts a DECIMAL to BIGNUM as an integer, so those go through GoogleSQL's conversions. A
// lambda takes the name _e and its `depth`.
std::string ParquetValue(const std::string& sql, const FieldSchema& field,
                         const ParquetColumn* column, int depth) {
  std::string cast = std::format("CAST({} AS {})", sql, ToDuckDbType(field));
  if (column == nullptr ||
      (field.type != FieldType::kBigNumeric && !HasType(field.fields, FieldType::kBigNumeric) &&
       !column->HasWideDecimal())) {
    return cast;
  }
  // Where the shapes differ, DuckDB's cast reports it.
  if (field.mode == FieldMode::kRepeated) {
    if (column->kind != ParquetColumn::Kind::kList) return cast;
    FieldSchema element = field;
    element.mode = FieldMode::kNullable;
    const std::string name = "_e" + std::to_string(depth);
    return std::format("list_transform({}, {} -> {})", sql, name,
                       ParquetValue(name, element, &column->children.front(), depth + 1));
  }
  if (field.type == FieldType::kRecord) {
    if (column->kind != ParquetColumn::Kind::kStruct) return cast;
    std::string fields;
    for (const FieldSchema& child : field.fields) {
      fields += std::format(
          "{}{} := {}", fields.empty() ? "" : ", ", QuoteIdentifier(child.name),
          ParquetValue(std::format("struct_extract({}, {})", sql, QuoteLiteral(child.name)), child,
                       column->Child(child.name), depth));
    }
    return std::format("CASE WHEN {0} IS NULL THEN NULL ELSE struct_pack({1}) END", sql, fields);
  }
  if (column->IsWideDecimal() &&
      (field.type != FieldType::kBigNumeric || column->scale > kBigNumericScale)) {
    throw ApiError::Invalid(
        std::format("{}loading a Parquet DECIMAL({}, {}) column into {} field {}", kUnsupported,
                    column->precision, column->scale, FieldTypeName(field.type), field.name));
  }
  if (field.type != FieldType::kBigNumeric) return cast;
  if (column->IsWideDecimal()) {
    return std::format("CAST(bq_bignumeric_from_decimal_bytes({}, {}) AS BIGNUM)", sql,
                       column->scale);
  }
  if (column->IsDecimal() || column->IsInteger()) {
    return std::format("CAST(bq_bignumeric_from_string(CAST({} AS VARCHAR), false) AS BIGNUM)",
                       sql);
  }
  throw ApiError::Invalid(std::format(
      "{}loading BIGNUMERIC field {} from a Parquet column that is not a DECIMAL or an integer",
      kUnsupported, field.name));
}

// The schema elements of the wide DECIMAL leaves of `column`.
void WideDecimals(const ParquetColumn& column, std::vector<size_t>& elements) {
  if (column.IsWideDecimal()) elements.push_back(column.element);
  for (const ParquetColumn& child : column.children) WideDecimals(child, elements);
}

// Gives `field` the type BigQuery detects for `column` when it is a wide DECIMAL. Of the
// `targets`, decimalTargetTypes, NUMERIC cannot hold one, and BIGNUMERIC is picked unless it
// cannot either and STRING is listed. Converting to NUMERIC, which is picked when it is the only
// one, or to STRING is unsupported.
void DetectDecimals(FieldSchema& field, const ParquetColumn* column, const json& targets) {
  if (column == nullptr) return;
  if (field.mode == FieldMode::kRepeated) {
    if (column->kind != ParquetColumn::Kind::kList) return;
    column = &column->children.front();
  }
  if (field.type == FieldType::kRecord) {
    for (FieldSchema& child : field.fields) {
      DetectDecimals(child, column->Child(child.name), targets);
    }
    return;
  }
  if (!column->IsWideDecimal()) return;
  const auto listed = [&targets](std::string_view type) {
    return std::ranges::find(targets, json(type)) != targets.end();
  };
  const bool fits = column->precision <= 76 && column->scale <= kBigNumericScale;
  if (!listed("BIGNUMERIC") || (!fits && listed("STRING"))) {
    throw ApiError::Invalid(std::format(
        "{}detecting the type of a Parquet DECIMAL({}, {}) column unless decimalTargetTypes picks "
        "BIGNUMERIC",
        kUnsupported, column->precision, column->scale));
  }
  field.type = FieldType::kBigNumeric;
}

}  // namespace

ParquetSources StageParquetDecimals(const std::vector<std::string>& paths,
                                    TemporaryFiles& downloads) {
  ParquetSources sources;
  for (const std::string& path : paths) {
    ThriftValue metadata = ReadParquetMetadata(path);
    const ParquetColumn columns = ParquetColumns(metadata);
    if (sources.paths.empty()) {
      sources.columns = columns;
    } else if (columns != sources.columns &&
               (columns.HasWideDecimal() || sources.columns.HasWideDecimal())) {
      throw ApiError::Invalid(std::string(kUnsupported) +
                              "loading Parquet files whose schemas differ when one has a DECIMAL "
                              "wider than 38 digits");
    }
    std::vector<size_t> wide;
    WideDecimals(columns, wide);
    if (wide.empty()) {
      sources.paths.push_back(path);
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
    sources.paths.push_back(staged);
  }
  return sources;
}

void DetectParquetDecimals(std::vector<FieldSchema>& schema, const ParquetColumn& columns,
                           const json& config) {
  json targets = config.value("decimalTargetTypes", json::array());
  if (!targets.is_array()) throw ApiError::Invalid("Invalid decimalTargetTypes");
  if (targets.empty()) targets.push_back("NUMERIC");
  for (FieldSchema& field : schema) DetectDecimals(field, columns.Child(field.name), targets);
}

std::vector<std::string> StageJsonNumerics(const std::vector<std::string>& paths,
                                           const std::vector<FieldSchema>& schema,
                                           TemporaryFiles& downloads) {
  if (!HasType(schema, FieldType::kNumeric) && !HasType(schema, FieldType::kBigNumeric)) {
    return paths;
  }
  std::vector<std::string> staged;
  for (const std::string& path : paths) {
    const std::string contents = ReadMaybeGzip(path);
    std::string rewritten;
    for (std::size_t start = 0; start < contents.size();) {
      std::size_t end = contents.find('\n', start);
      if (end == std::string::npos) end = contents.size();
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
    staged.push_back(downloads.Write(rewritten));
  }
  return staged;
}

std::string LoadQuery(const std::string& format, const std::vector<std::string>& paths,
                      const json& config, const std::vector<FieldSchema>& schema,
                      const ParquetColumn& parquet) {
  std::string files;
  for (const std::string& path : paths) {
    files += (files.empty() ? "" : ", ") + QuoteLiteral(path);
  }
  files = "[" + files + "]";
  std::string sql;
  if (format == "CSV") {
    sql = "SELECT * FROM read_csv(" + files +
          ", header=false, skip=" + std::to_string(config.value("skipLeadingRows", 0)) +
          ", delim=" + QuoteLiteral(config.value("fieldDelimiter", ","));
    if (!schema.empty()) {
      sql += ", auto_detect=false";
      std::string columns;
      // read_csv() would truncate a BIGNUM, so a BIGNUMERIC is read as text and converted.
      std::string conversions;
      for (const FieldSchema& field : schema) {
        const bool big = field.type == FieldType::kBigNumeric;
        columns += (columns.empty() ? "" : ", ") + QuoteLiteral(field.name) + ": " +
                   QuoteLiteral(big ? "VARCHAR" : ToDuckDbType(field));
        conversions +=
            (conversions.empty() ? "" : ", ") +
            (big ? std::format("CAST(bq_bignumeric_from_string({0}, false) AS {1}) AS {0}",
                               QuoteIdentifier(field.name), ToDuckDbType(field))
                 : QuoteIdentifier(field.name));
      }
      sql += ", columns={" + columns + "})";
      return std::format("SELECT {} FROM ({}) AS source", conversions, sql);
    }
    sql += ')';
  } else if (format == "NEWLINE_DELIMITED_JSON") {
    sql = std::format("SELECT * FROM read_json({}, format='newline_delimited')", files);
  } else {
    sql = std::format("SELECT * FROM read_parquet({})", files);
  }
  if (!schema.empty() && format != "CSV") {
    std::string columns;
    for (const FieldSchema& field : schema) {
      if (!columns.empty()) columns += ", ";
      const std::string name = QuoteIdentifier(field.name);
      columns += std::format("{} AS {}",
                             format == "PARQUET"
                                 ? ParquetValue(name, field, parquet.Child(field.name), 0)
                                 : std::format("CAST({} AS {})", name, ToDuckDbType(field)),
                             name);
    }
    sql = std::format("SELECT {} FROM ({}) AS source", columns, sql);
  }
  return sql;
}

}  // namespace bigquery_emulator_duckdb
