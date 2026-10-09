#include "src/extract.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
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
#include "src/field_schema.h"
#include "src/parquet_metadata.h"
#include "src/type_mapping.h"

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
    if (field.type == FieldType::kInterval || field.type == FieldType::kRange ||
        field.type == FieldType::kGeography) {
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
    const FieldSchema& field = fields.at(i);
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

// A copy of `field` whose DuckDB type is the one Parquet export writes it as, except that a
// BIGNUMERIC stays one; see ParquetValue.
FieldSchema ParquetField(FieldSchema field) {
  switch (field.type) {
    case FieldType::kInterval:
    case FieldType::kRange:
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

bool HasBigNumeric(const FieldSchema& field) {
  return field.type == FieldType::kBigNumeric || std::ranges::any_of(field.fields, HasBigNumeric);
}

// The DuckDB value `sql` of `field`, a ParquetField, as Parquet export writes it. DuckDB cannot
// write a DECIMAL wider than 38 digits, so a BIGNUMERIC is written as the bytes of a
// DECIMAL(76, 38), which AnnotateParquetBigNumerics then declares in the footer. A lambda takes
// the name _e and its `depth`.
std::string ParquetValue(const std::string& sql, const FieldSchema& field, int depth) {
  if (!HasBigNumeric(field)) return std::format("CAST({} AS {})", sql, ParquetType(field));
  if (field.mode == FieldMode::kRepeated) {
    FieldSchema element = field;
    element.mode = FieldMode::kNullable;
    const std::string name = "_e" + std::to_string(depth);
    return std::format("list_transform({}, {} -> {})", sql, name,
                       ParquetValue(name, element, depth + 1));
  }
  if (field.type == FieldType::kBigNumeric) {
    return std::format("bq_bignumeric_to_decimal_bytes(CAST({} AS VARCHAR))", sql);
  }
  std::string fields;
  for (const FieldSchema& child : field.fields) {
    fields += std::format(
        "{}{} := {}", fields.empty() ? "" : ", ", QuoteIdentifier(child.name),
        ParquetValue(std::format("struct_extract({}, {})", sql, QuoteLiteral(child.name)), child,
                     depth));
  }
  return std::format("CASE WHEN {0} IS NULL THEN NULL ELSE struct_pack({1}) END", sql, fields);
}

// Declares the leaves of `column` that hold the BIGNUMERICs of `field` as DECIMAL(76, 38) in
// `schema`, FileMetaData's schema elements, adding their indexes to `leaves`.
void AnnotateBigNumerics(const FieldSchema& field, const ParquetColumn& column,
                         std::vector<ThriftValue>& schema, std::vector<size_t>& leaves) {
  if (field.mode == FieldMode::kRepeated) {
    FieldSchema element = field;
    element.mode = FieldMode::kNullable;
    AnnotateBigNumerics(element, column.children.at(0), schema, leaves);
  } else if (field.type == FieldType::kRecord) {
    for (size_t i = 0; i < field.fields.size(); ++i) {
      AnnotateBigNumerics(field.fields.at(i), column.children.at(i), schema, leaves);
    }
  } else if (field.type == FieldType::kBigNumeric) {
    namespace pq = parquet;
    ThriftValue& element = schema.at(column.element);
    element.SetField(pq::kElementConvertedType, ThriftValue::Int32(pq::kConvertedDecimal));
    element.SetField(pq::kElementScale, ThriftValue::Int32(38));
    element.SetField(pq::kElementPrecision, ThriftValue::Int32(76));
    element.SetField(pq::kElementLogicalType,
                     ThriftValue::Struct({
                         {
                             pq::kLogicalDecimal,
                             ThriftValue::Struct({
                                 {pq::kDecimalScale, ThriftValue::Int32(38)},
                                 {pq::kDecimalPrecision, ThriftValue::Int32(76)},
                             }),
                         },
                     }));
    leaves.push_back(column.element);
  }
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
    const auto nested = std::ranges::find_if(schema, [](const FieldSchema& field) {
      return field.type == FieldType::kRecord || field.mode == FieldMode::kRepeated;
    });
    if (nested != schema.end()) {
      throw ApiError::Invalid("Operation cannot be performed on a nested schema. Field: " +
                              nested->name);
    }
    const std::string& delimiter = options.field_delimiter;
    if (options.print_header) {
      for (size_t i = 0; i < schema.size(); ++i) {
        if (i > 0) contents += delimiter;
        AppendCsvField(schema.at(i).name, delimiter, contents);
      }
      contents += '\n';
    }
    for (const json& row : rows) {
      const json& cells = row.at("f");
      for (size_t i = 0; i < schema.size(); ++i) {
        if (i > 0) contents += delimiter;
        const json& value = cells.at(i).at("v");
        if (!value.is_null()) {
          AppendCsvField(ScalarText(schema.at(i), value.get<std::string>()), delimiter, contents);
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
    columns += std::format("{} AS {}", ParquetValue(name, ParquetField(field), 0), name);
  }
  return columns;
}

void AnnotateParquetBigNumerics(const std::string& path, const std::vector<FieldSchema>& schema) {
  if (std::ranges::none_of(schema, HasBigNumeric)) return;
  ThriftValue metadata = ReadParquetMetadata(path);
  const ParquetColumn columns = ParquetColumns(metadata);
  std::vector<ThriftValue>& elements = metadata.Field(parquet::kSchema)->elements;
  std::vector<size_t> leaves;
  for (size_t i = 0; i < schema.size(); ++i) {
    AnnotateBigNumerics(schema.at(i), columns.children.at(i), elements, leaves);
  }
  // A row group has a column chunk for each leaf, in the schema's order. The statistics, column
  // index and bloom filter DuckDB wrote compare and hash the bytes as BYTES, which readers of a
  // DECIMAL would misread, so they go.
  std::vector<size_t> chunks;
  size_t leaf = 0;
  for (size_t i = 0; i < elements.size(); ++i) {
    const ThriftValue* children = elements.at(i).Field(parquet::kElementChildren);
    if (children != nullptr && children->integer > 0) continue;
    if (std::ranges::find(leaves, i) != leaves.end()) chunks.push_back(leaf);
    ++leaf;
  }
  if (ThriftValue* groups = metadata.Field(parquet::kRowGroups)) {
    for (ThriftValue& group : groups->elements) {
      std::vector<ThriftValue>& group_chunks = group.Field(parquet::kRowGroupColumns)->elements;
      for (const size_t chunk : chunks) {
        ThriftValue& column = group_chunks.at(chunk);
        column.RemoveField(parquet::kChunkColumnIndexOffset);
        column.RemoveField(parquet::kChunkColumnIndexLength);
        if (ThriftValue* meta = column.Field(parquet::kChunkMetadata)) {
          meta->RemoveField(parquet::kColumnStatistics);
          meta->RemoveField(parquet::kColumnBloomFilterOffset);
          meta->RemoveField(parquet::kColumnBloomFilterLength);
        }
      }
    }
  }
  RewriteParquetMetadata(path, metadata);
}

}  // namespace bigquery_emulator_duckdb
