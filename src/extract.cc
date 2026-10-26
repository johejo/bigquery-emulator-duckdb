#include "src/extract.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/time/time.h"
#include "googlesql/public/functions/convert_string.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/extract_options.h"
#include "src/field_schema.h"
#include "src/parquet_metadata.h"
#include "src/schema_sql.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {

namespace {

template <typename T>
T Checked(absl::StatusOr<T> value) {
  if (!value.ok()) {
    throw ApiError::Invalid(std::string(value.status().message()));
  }
  return *std::move(value);
}

void Check(const absl::Status& status) {
  if (!status.ok()) {
    throw ApiError::Invalid(std::string(status.message()));
  }
}

// A DATETIME or TIME as query results spell it, without trailing zeros in the fraction.
std::string TrimFraction(std::string text) {
  if (text.find('.') != std::string::npos) {
    text.erase(text.find_last_not_of('0') + 1);
    if (text.back() == '.') {
      text.pop_back();
    }
  }
  return text;
}

// The text CAST(value AS STRING) gives a scalar, except that BYTES is base64, TIMESTAMP is spelled
// as BigQuery exports it, and DATETIME and TIME as query results spell them. JSON is serialized
// as GoogleSQL does.
std::string ScalarText(const googlesql::Value& value) {
  std::string out;
  switch (value.type_kind()) {
    case googlesql::TYPE_STRING:
      return value.string_value();
    case googlesql::TYPE_BYTES:
      return absl::Base64Escape(value.bytes_value());
    case googlesql::TYPE_BOOL:
      return value.bool_value() ? "true" : "false";
    case googlesql::TYPE_INT64:
      return std::to_string(value.int64_value());
    case googlesql::TYPE_DOUBLE: {
      absl::Status error;
      if (!googlesql::functions::NumericToString(value.double_value(), &out, &error, false)) {
        Check(error);
      }
      return out;
    }
    case googlesql::TYPE_NUMERIC:
      return value.numeric_value().ToString();
    case googlesql::TYPE_BIGNUMERIC:
      return value.bignumeric_value().ToString();
    case googlesql::TYPE_DATE:
      Check(googlesql::functions::ConvertDateToString(value.date_value(), &out));
      return out;
    case googlesql::TYPE_TIMESTAMP:
      Check(googlesql::functions::FormatTimestampToString(
          "%Y-%m-%d %H:%M:%E*S UTC", value.ToUnixPicos().ToAbslTime(), absl::UTCTimeZone(), &out));
      return out;
    case googlesql::TYPE_DATETIME:
      Check(googlesql::functions::ConvertDatetimeToString(
          value.datetime_value(), googlesql::functions::kMicroseconds, &out));
      out.at(10) = 'T';
      return TrimFraction(std::move(out));
    case googlesql::TYPE_TIME:
      Check(googlesql::functions::ConvertTimeToString(value.time_value(),
                                                      googlesql::functions::kMicroseconds, &out));
      return TrimFraction(std::move(out));
    case googlesql::TYPE_JSON:
      return value.json_value().ToString();
    default:
      throw ApiError::Unsupported("extracting " + value.type()->DebugString() + " values");
  }
}

void CheckTextFields(const std::vector<FieldSchema>& fields) {
  for (const FieldSchema& field : fields) {
    if (field.type == FieldType::kInterval || field.type == FieldType::kRange ||
        field.type == FieldType::kGeography) {
      throw ApiError::Unsupported("extracting " + std::string(FieldTypeName(field.type)) +
                                  " columns");
    }
    CheckTextFields(field.fields);
  }
}

// The JSON object of a row or RECORD whose fields are `fields` and whose field `i` is
// value_at(i). NULL fields are left out.
template <typename F>
void AppendJsonObject(const std::vector<FieldSchema>& fields, const F& value_at, std::string& out);

// One non-NULL value of `field`: an element of a REPEATED field or a whole scalar or RECORD.
// INT64, NUMERIC and BIGNUMERIC are strings, so that readers keep their precision.
void AppendJsonValue(const FieldSchema& field, const googlesql::Value& value, std::string& out) {
  switch (field.type) {
    case FieldType::kRecord:
      AppendJsonObject(
          field.fields,
          [&](size_t i) -> const googlesql::Value& { return value.field(static_cast<int>(i)); },
          out);
      return;
    case FieldType::kBoolean:
    case FieldType::kJson:
      out += ScalarText(value);
      return;
    case FieldType::kFloat: {
      const double number = value.double_value();
      // TO_JSON's spelling of the values JSON has no number for.
      out += std::isnan(number)   ? "\"NaN\""
             : std::isinf(number) ? (number > 0 ? "\"Infinity\"" : "\"-Infinity\"")
                                  : googlesql::JSONValue(number).GetConstRef().ToString();
      return;
    }
    default:
      out += googlesql::JSONValue(ScalarText(value)).GetConstRef().ToString();
  }
}

template <typename F>
void AppendJsonObject(const std::vector<FieldSchema>& fields, const F& value_at, std::string& out) {
  out += '{';
  bool first = true;
  for (size_t i = 0; i < fields.size(); ++i) {
    const FieldSchema& field = fields.at(i);
    const googlesql::Value& value = value_at(i);
    if (value.is_null()) {
      continue;
    }
    if (!first) {
      out += ',';
    }
    first = false;
    out += googlesql::JSONValue(field.name).GetConstRef().ToString();
    out += ':';
    if (field.mode != FieldMode::kRepeated) {
      AppendJsonValue(field, value, out);
      continue;
    }
    out += '[';
    for (int j = 0; j < value.num_elements(); ++j) {
      if (j > 0) {
        out += ',';
      }
      AppendJsonValue(field, value.element(j), out);
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
    if (!output) {
      throw ApiError::Invalid("Could not write " + path);
    }
    return;
  }
  gzFile output = gzopen(path.c_str(), "wb");
  if (output == nullptr) {
    throw ApiError::Invalid("Could not write " + path);
  }
  const bool written = contents.empty() ||
                       gzwrite(output, contents.data(), static_cast<unsigned>(contents.size())) > 0;
  if (gzclose(output) != Z_OK || !written) {
    throw ApiError::Invalid("Could not write " + path);
  }
}

// A copy of `field` whose DuckDB type is the one Parquet export writes it as, except that a
// BIGNUMERIC stays one; see ParquetValue.
FieldSchema ParquetField(FieldSchema field) {
  switch (field.type) {
    case FieldType::kInterval:
    case FieldType::kRange:
    case FieldType::kGeography:
      throw ApiError::Unsupported("extracting " + std::string(FieldTypeName(field.type)) +
                                  " columns to Parquet");
    // TIMESTAMP is written without isAdjustedToUTC, which is DuckDB's TIMESTAMP, and JSON as a
    // string.
    case FieldType::kTimestamp:
      field.type = FieldType::kDatetime;
      break;
    case FieldType::kJson:
      field.type = FieldType::kString;
      break;
    case FieldType::kRecord:
      for (FieldSchema& child : field.fields) {
        child = ParquetField(std::move(child));
      }
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
    std::vector<std::string> fields;
    std::ranges::transform(field.fields, std::back_inserter(fields), [&](const FieldSchema& child) {
      return QuoteIdentifier(child.name) + " " + ParquetType(child);
    });
    type = "STRUCT(" + Join(fields, ", ") + ")";
  } else {
    type = Checked(DuckDbColumnType(AsNullable(field)));
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
  if (!HasBigNumeric(field)) {
    return std::format("CAST({} AS {})", sql, ParquetType(field));
  }
  if (field.mode == FieldMode::kRepeated) {
    return MapElements(sql, field, depth,
                       [](const std::string& name, const FieldSchema& element, int element_depth) {
                         return ParquetValue(name, element, element_depth);
                       });
  }
  if (field.type == FieldType::kBigNumeric) {
    return std::format("bq_bignumeric_to_decimal_bytes(CAST({} AS VARCHAR))", sql);
  }
  return MapFields(sql, field, [&](const std::string& value, const FieldSchema& child) {
    return ParquetValue(value, child, depth);
  });
}

// Declares the leaves of `column` that hold the BIGNUMERICs of `field` as DECIMAL(76, 38) in
// `schema`, FileMetaData's schema elements, adding their indexes to `leaves`.
void AnnotateBigNumerics(const FieldSchema& field, const ParquetColumn& column,
                         std::vector<ThriftValue>& schema, std::vector<size_t>& leaves) {
  if (field.mode == FieldMode::kRepeated) {
    AnnotateBigNumerics(AsNullable(field), column.children.at(0), schema, leaves);
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
                             .id = pq::kLogicalDecimal,
                             .value = ThriftValue::Struct({
                                 {.id = pq::kDecimalScale, .value = ThriftValue::Int32(38)},
                                 {.id = pq::kDecimalPrecision, .value = ThriftValue::Int32(76)},
                             }),
                         },
                     }));
    leaves.push_back(column.element);
  }
}

}  // namespace

void WriteTextExtract(const std::vector<FieldSchema>& schema, size_t row_count,
                      const ExtractValue& value, const TextExtractFormatOptions& options,
                      ExtractCompression compression, const std::string& path) {
  if (compression != ExtractCompression::kNone && compression != ExtractCompression::kGzip) {
    throw ApiError::Invalid("Unsupported text extract compression: " +
                            std::string(ExtractCompressionName(compression)));
  }
  CheckTextFields(schema);
  googlesql::TypeFactory type_factory;
  std::vector<const googlesql::Type*> types;
  std::ranges::transform(schema, std::back_inserter(types), [&](const FieldSchema& field) {
    return Checked(GoogleSqlType(field, &type_factory));
  });
  const auto row = [&](size_t r) {
    std::vector<googlesql::Value> values;
    values.reserve(types.size());
    for (size_t i = 0; i < types.size(); ++i) {
      values.push_back(Checked(value(r, i, types.at(i))));
    }
    return values;
  };
  std::string contents;
  if (std::holds_alternative<JsonExtractOptions>(options)) {
    for (size_t r = 0; r < row_count; ++r) {
      std::string line;
      const std::vector<googlesql::Value> values = row(r);
      AppendJsonObject(
          schema, [&](size_t i) -> const googlesql::Value& { return values.at(i); }, line);
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
    const auto& csv = std::get<CsvExtractOptions>(options);
    const std::string& delimiter = csv.field_delimiter;
    if (csv.print_header) {
      for (size_t i = 0; i < schema.size(); ++i) {
        if (i > 0) {
          contents += delimiter;
        }
        AppendCsvField(schema.at(i).name, delimiter, contents);
      }
      contents += '\n';
    }
    for (size_t r = 0; r < row_count; ++r) {
      const std::vector<googlesql::Value> values = row(r);
      for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
          contents += delimiter;
        }
        if (!values.at(i).is_null()) {
          AppendCsvField(ScalarText(values.at(i)), delimiter, contents);
        }
      }
      contents += '\n';
    }
  }
  WriteFile(path, contents, compression == ExtractCompression::kGzip);
}

std::string ParquetExtractColumns(const std::vector<FieldSchema>& schema) {
  std::vector<std::string> columns;
  for (const FieldSchema& field : schema) {
    const std::string name = QuoteIdentifier(field.name);
    columns.push_back(std::format("{} AS {}", ParquetValue(name, ParquetField(field), 0), name));
  }
  return Join(columns, ", ");
}

void AnnotateParquetBigNumerics(const std::string& path, const std::vector<FieldSchema>& schema) {
  if (std::ranges::none_of(schema, HasBigNumeric)) {
    return;
  }
  ThriftValue metadata = ReadParquetMetadata(path);
  // DuckDB writes a REPEATED field as a standard LIST group.
  const ParquetColumn columns = ParquetColumns(metadata, /*list_inference=*/true);
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
    if (children != nullptr && children->integer > 0) {
      continue;
    }
    if (std::ranges::find(leaves, i) != leaves.end()) {
      chunks.push_back(leaf);
    }
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
