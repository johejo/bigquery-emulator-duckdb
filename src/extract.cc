#include "src/extract.h"

#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "googlesql/public/functions/convert_string.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/numeric_value.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/type_mapping.h"
#include "zlib.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

namespace {

constexpr std::string_view kUnsupported = "The emulator does not support ";

template <typename T>
T Checked(absl::StatusOr<T> value) {
  if (!value.ok()) throw ApiError::Invalid(std::string(value.status().message()));
  return *std::move(value);
}

void Check(const absl::Status& status) {
  if (!status.ok()) throw ApiError::Invalid(std::string(status.message()));
}

// The text CAST(value AS STRING) gives a scalar, or for TIMESTAMP the spelling BigQuery exports,
// from its wire format. JSON is serialized as GoogleSQL does.
std::string ScalarText(const FieldSchema& field, const std::string& value) {
  std::string out;
  switch (field.type) {
    case FieldType::kFloat: {
      absl::Status error;
      if (!googlesql::functions::NumericToString(std::strtod(value.c_str(), nullptr), &out, &error,
                                                 false)) {
        Check(error);
      }
      return out;
    }
    case FieldType::kNumeric:
      return Checked(googlesql::NumericValue::FromString(value)).ToString();
    case FieldType::kBigNumeric:
      return Checked(googlesql::BigNumericValue::FromString(value)).ToString();
    case FieldType::kTimestamp:
      Check(googlesql::functions::FormatTimestampToString(
          "%Y-%m-%d %H:%M:%E*S UTC", std::stoll(value), absl::UTCTimeZone(), &out));
      return out;
    case FieldType::kJson:
      return Checked(googlesql::JSONValue::ParseJSONString(value)).GetConstRef().ToString();
    default:
      // STRING, BYTES (already base64), INT64, BOOL, DATE, TIME and DATETIME.
      return value;
  }
}

void CheckTextFields(const std::vector<FieldSchema>& fields) {
  for (const FieldSchema& field : fields) {
    if (field.type == FieldType::kInterval || field.type == FieldType::kGeography) {
      throw ApiError::Invalid(std::string(kUnsupported) + "extracting " +
                              std::string(FieldTypeName(field.type)) + " columns");
    }
    CheckTextFields(field.fields);
  }
}

void AppendJsonObject(const std::vector<FieldSchema>& fields, const json& cells, std::string& out);

// One non-NULL value of `field`: an element of a REPEATED field or a whole scalar or RECORD.
// INT64, NUMERIC and BIGNUMERIC are strings, so that readers keep their precision.
void AppendJsonValue(const FieldSchema& field, const json& value, std::string& out) {
  switch (field.type) {
    case FieldType::kRecord:
      AppendJsonObject(field.fields, value.at("f"), out);
      return;
    case FieldType::kBoolean:
      out += value.get<std::string>();
      return;
    case FieldType::kJson:
      out += ScalarText(field, value.get<std::string>());
      return;
    case FieldType::kFloat: {
      const double number = std::strtod(value.get<std::string>().c_str(), nullptr);
      // TO_JSON's spelling of the values JSON has no number for.
      out += std::isnan(number)   ? "\"NaN\""
             : std::isinf(number) ? (number > 0 ? "\"Infinity\"" : "\"-Infinity\"")
                                  : googlesql::JSONValue(number).GetConstRef().ToString();
      return;
    }
    default:
      out += googlesql::JSONValue(ScalarText(field, value.get<std::string>()))
                 .GetConstRef()
                 .ToString();
  }
}

// NULL fields are left out.
void AppendJsonObject(const std::vector<FieldSchema>& fields, const json& cells, std::string& out) {
  out += '{';
  bool first = true;
  for (size_t i = 0; i < fields.size(); ++i) {
    const FieldSchema& field = fields[i];
    const json& value = cells.at(i).at("v");
    if (value.is_null()) continue;
    if (!first) out += ',';
    first = false;
    out += googlesql::JSONValue(field.name).GetConstRef().ToString();
    out += ':';
    if (field.mode != FieldMode::kRepeated) {
      AppendJsonValue(field, value, out);
      continue;
    }
    out += '[';
    for (size_t j = 0; j < value.size(); ++j) {
      if (j > 0) out += ',';
      AppendJsonValue(field, value[j].at("v"), out);
    }
    out += ']';
  }
  out += '}';
}

// Escapes <, > and & as BigQuery's JSON export does. They only occur inside strings.
std::string EscapeHtml(const std::string& line) {
  std::string out;
  for (const char c : line) {
    out += c == '<' ? "\\u003c" : c == '>' ? "\\u003e" : c == '&' ? "\\u0026" : std::string(1, c);
  }
  return out;
}

void AppendCsvField(const std::string& value, const std::string& delimiter, std::string& out) {
  if (value.find(delimiter) == std::string::npos &&
      value.find_first_of("\"\r\n") == std::string::npos) {
    out += value;
    return;
  }
  out += '"';
  for (const char c : value) {
    out += c == '"' ? "\"\"" : std::string(1, c);
  }
  out += '"';
}

void WriteFile(const std::string& path, std::string_view contents, bool gzip) {
  if (!gzip) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) throw ApiError::Invalid("Could not write " + path);
    return;
  }
  gzFile output = gzopen(path.c_str(), "wb");
  if (output == nullptr) throw ApiError::Invalid("Could not write " + path);
  const bool written = contents.empty() ||
                       gzwrite(output, contents.data(), static_cast<unsigned>(contents.size())) > 0;
  if (gzclose(output) != Z_OK || !written) throw ApiError::Invalid("Could not write " + path);
}

// A copy of `field` whose DuckDB type is the one Parquet export writes it as.
FieldSchema ParquetField(FieldSchema field) {
  switch (field.type) {
    case FieldType::kBigNumeric:
    case FieldType::kInterval:
    case FieldType::kGeography:
      throw ApiError::Invalid(std::string(kUnsupported) + "extracting " +
                              std::string(FieldTypeName(field.type)) + " columns to Parquet");
    // TIMESTAMP is written without isAdjustedToUTC, which is DuckDB's TIMESTAMP, and JSON as a
    // string.
    case FieldType::kTimestamp:
      field.type = FieldType::kDatetime;
      break;
    case FieldType::kJson:
      field.type = FieldType::kString;
      break;
    case FieldType::kRecord:
      for (FieldSchema& child : field.fields) child = ParquetField(std::move(child));
      break;
    default:
      break;
  }
  return field;
}

// TIME is written with isAdjustedToUTC, which DuckDB does only for TIMETZ.
std::string ParquetType(const FieldSchema& field) {
  std::string type;
  if (field.type == FieldType::kTime) {
    type = "TIMETZ";
  } else if (field.type == FieldType::kRecord) {
    for (const FieldSchema& child : field.fields) {
      type += (type.empty() ? "" : ", ") + QuoteIdentifier(child.name) + " " + ParquetType(child);
    }
    type = "STRUCT(" + type + ")";
  } else {
    FieldSchema scalar = field;
    scalar.mode = FieldMode::kNullable;
    type = Checked(DuckDbColumnType(scalar));
  }
  return field.mode == FieldMode::kRepeated ? type + "[]" : type;
}

}  // namespace

void WriteTextExtract(const std::vector<FieldSchema>& schema, const std::vector<json>& rows,
                      const TextExtractOptions& options, const std::string& path) {
  CheckTextFields(schema);
  std::string contents;
  if (options.json) {
    for (const json& row : rows) {
      std::string line;
      AppendJsonObject(schema, row.at("f"), line);
      contents += EscapeHtml(line) + "\n";
    }
  } else {
    for (const FieldSchema& field : schema) {
      if (field.type == FieldType::kRecord || field.mode == FieldMode::kRepeated) {
        throw ApiError::Invalid("Operation cannot be performed on a nested schema. Field: " +
                                field.name);
      }
    }
    const std::string& delimiter = options.field_delimiter;
    if (options.print_header) {
      for (size_t i = 0; i < schema.size(); ++i) {
        if (i > 0) contents += delimiter;
        AppendCsvField(schema[i].name, delimiter, contents);
      }
      contents += '\n';
    }
    for (const json& row : rows) {
      const json& cells = row.at("f");
      for (size_t i = 0; i < schema.size(); ++i) {
        if (i > 0) contents += delimiter;
        const json& value = cells.at(i).at("v");
        if (!value.is_null()) {
          AppendCsvField(ScalarText(schema[i], value.get<std::string>()), delimiter, contents);
        }
      }
      contents += '\n';
    }
  }
  WriteFile(path, contents, options.gzip);
}

std::string ParquetExtractColumns(const std::vector<FieldSchema>& schema) {
  std::string columns;
  for (const FieldSchema& field : schema) {
    const std::string name = QuoteIdentifier(field.name);
    if (!columns.empty()) columns += ", ";
    columns += std::format("CAST({0} AS {1}) AS {0}", name, ParquetType(ParquetField(field)));
  }
  return columns;
}

}  // namespace bigquery_emulator_duckdb
