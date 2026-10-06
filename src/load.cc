#include "src/load.h"

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
#include "src/schema_sql.h"
#include "src/temporary_files.h"
#include "zlib.h"

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
  std::string buffer(1 << 16, '\0');
  int read = 0;
  while ((read = gzread(input, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0) {
    contents.append(buffer, 0, static_cast<std::size_t>(read));
  }
  const bool ok = read == 0;
  gzclose(input);
  if (!ok) throw ApiError::Invalid("Could not read " + path);
  return contents;
}

}  // namespace

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
                      const json& config, const std::vector<FieldSchema>& schema) {
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
    sql += ")";
  } else if (format == "NEWLINE_DELIMITED_JSON") {
    sql = std::format("SELECT * FROM read_json({}, format='newline_delimited')", files);
  } else if (HasType(schema, FieldType::kBigNumeric)) {
    // read_parquet() reads a DECIMAL wider than 38 digits as a DOUBLE.
    throw ApiError::Invalid("Loading BIGNUMERIC from " + format + " is not supported");
  } else {
    sql = std::format("SELECT * FROM read_parquet({})", files);
  }
  if (!schema.empty() && format != "CSV") {
    std::string columns;
    for (const FieldSchema& field : schema) {
      if (!columns.empty()) columns += ", ";
      columns +=
          std::format("CAST({0} AS {1}) AS {0}", QuoteIdentifier(field.name), ToDuckDbType(field));
    }
    sql = std::format("SELECT {} FROM ({}) AS source", columns, sql);
  }
  return sql;
}

}  // namespace bigquery_emulator_duckdb
