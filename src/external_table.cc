#include "src/external_table.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "src/api_error.h"
#include "src/csv_detect.h"
#include "src/csv_records.h"
#include "src/csv_stage.h"
#include "src/datetime_formats.h"
#include "src/duckdb_sql.h"
#include "src/external_json.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/file_read_options.h"
#include "src/gcs.h"
#include "src/hive_partitioning.h"
#include "src/json_detect.h"
#include "src/parquet_metadata.h"
#include "src/schema_sql.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"
#include "src/temporary_files.h"
#include "src/text_encoding.h"

namespace bigquery_emulator_duckdb {
namespace {

// The LogicalType union member of `element`, a SchemaElement, and its value, if it has one.
std::pair<int16_t, const ThriftValue*> Logical(const ThriftValue& element) {
  const ThriftValue* logical = element.Field(parquet::kElementLogicalType);
  if (logical == nullptr || logical->fields.empty()) {
    return {0, nullptr};
  }
  return {logical->fields.front().id, &logical->fields.front().value};
}

// The TimeUnit member of a TIME or TIMESTAMP `element`, from its LogicalType or ConvertedType.
int16_t TimeUnit(const ThriftValue& element, const ParquetColumn& column) {
  if (const auto [member, value] = Logical(element); value != nullptr) {
    const ThriftValue* unit = value->Field(parquet::kTimeUnit);
    return unit == nullptr || unit->fields.empty() ? 0 : unit->fields.front().id;
  }
  switch (column.converted) {
    case parquet::kConvertedTimeMillis:
    case parquet::kConvertedTimestampMillis:
      return parquet::kTimeUnitMillis;
    case parquet::kConvertedTimeMicros:
    case parquet::kConvertedTimestampMicros:
      return parquet::kTimeUnitMicros;
    default:
      return 0;
  }
}

// Whether the LogicalType of `element`, a TIME or TIMESTAMP, is adjusted to UTC.
bool AdjustedToUtc(const ThriftValue& element) {
  const auto [member, value] = Logical(element);
  const ThriftValue* adjusted =
      value == nullptr ? nullptr : value->Field(parquet::kTimeAdjustedToUtc);
  return adjusted != nullptr && adjusted->type == ThriftValue::Type::kTrue;
}

// The type BigQuery detects for the Parquet leaf `column`, whose SchemaElement is `element`, as
// the Parquet conversions of
// [Loading Parquet data](third_party/bigquery_docs/loading-data-cloud-storage-parquet.md)
// list them, or none for other combinations, which BigQuery does not convert, and for ENUM and
// GeoParquet, whose types depend on options the emulator does not take. A DECIMAL takes the
// default decimalTargetTypes, NUMERIC; DetectGcsExternalSchema detects a top-level one that NUMERIC
// does not hold.
std::optional<FieldType> DetectedType(const ThriftValue& element, const ParquetColumn& column) {
  if (column.kind != ParquetColumn::Kind::kLeaf || column.edges != ParquetColumn::Edges::kNone) {
    return std::nullopt;
  }
  if (column.IsDecimal()) {
    const bool physical = column.type == parquet::kInt32 || column.type == parquet::kInt64 ||
                          column.type == parquet::kFixedLenByteArray;
    if (physical && column.HoldsNumeric()) {
      return FieldType::kNumeric;
    }
    return std::nullopt;
  }
  const int16_t logical = Logical(element).first;
  const bool plain = column.converted == -1 && logical == 0;
  const bool date = logical == parquet::kLogicalDate || column.converted == parquet::kConvertedDate;
  const bool time = logical == parquet::kLogicalTime ||
                    column.converted == parquet::kConvertedTimeMillis ||
                    column.converted == parquet::kConvertedTimeMicros;
  const bool timestamp = logical == parquet::kLogicalTimestamp ||
                         column.converted == parquet::kConvertedTimestampMillis ||
                         column.converted == parquet::kConvertedTimestampMicros;
  const bool utf8 =
      logical == parquet::kLogicalString || column.converted == parquet::kConvertedUtf8;
  switch (column.type) {
    case parquet::kBoolean:
      return plain ? std::optional(FieldType::kBoolean) : std::nullopt;
    case parquet::kInt32:
      if (column.IsInteger()) {
        return FieldType::kInteger;
      }
      if (date) {
        return FieldType::kDate;
      }
      if (time && TimeUnit(element, column) == parquet::kTimeUnitMillis) {
        return FieldType::kTime;
      }
      return std::nullopt;
    case parquet::kInt64:
      if (column.IsInteger()) {
        return FieldType::kInteger;
      }
      if (time && TimeUnit(element, column) == parquet::kTimeUnitMicros) {
        return FieldType::kTime;
      }
      if (timestamp && (TimeUnit(element, column) == parquet::kTimeUnitMillis ||
                        TimeUnit(element, column) == parquet::kTimeUnitMicros)) {
        return FieldType::kTimestamp;
      }
      // As BigQuery's answers in external_table_parquet_types.api.answers.json show, it reads a
      // TIMESTAMP of nanoseconds, not adjusted to UTC, as its INT64 value.
      if (timestamp && TimeUnit(element, column) == parquet::kTimeUnitNanos &&
          !AdjustedToUtc(element)) {
        return FieldType::kInteger;
      }
      return std::nullopt;
    case parquet::kInt96:
      return plain ? std::optional(FieldType::kTimestamp) : std::nullopt;
    case parquet::kFloat:
    case parquet::kDouble:
      return plain ? std::optional(FieldType::kFloat) : std::nullopt;
    case parquet::kByteArray:
      if (plain) {
        return FieldType::kBytes;
      }
      return utf8 ? std::optional(FieldType::kString) : std::nullopt;
    case parquet::kFixedLenByteArray:
      return plain ? std::optional(FieldType::kBytes) : std::nullopt;
    default:
      return std::nullopt;
  }
}

const ThriftValue& Element(const ThriftValue& metadata, const ParquetColumn& column) {
  return metadata.Field(parquet::kSchema)->elements.at(column.element);
}

// Whether `metadata`, a FileMetaData, has GeoParquet metadata, whose geometry columns BigQuery
// detects as GEOGRAPHY.
bool HasGeoParquet(const ThriftValue& metadata) {
  // FileMetaData.key_value_metadata (5), KeyValue.key (1).
  const ThriftValue* entries = metadata.Field(5);
  return entries != nullptr && std::ranges::any_of(entries->elements, [](const ThriftValue& entry) {
           const ThriftValue* key = entry.Field(1);
           return key != nullptr && key->bytes == "geo";
         });
}

// Whether `name` is a column name of letters, digits and underscores that BigQuery takes as it
// is. Flexible column names and BigQuery's reserved prefixes are not emulated.
bool StandardColumnName(std::string_view name) {
  std::string upper;
  std::ranges::transform(name, std::back_inserter(upper),
                         [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  constexpr std::string_view kReserved[] = {
      "_TABLE_",
      "_FILE_",
      "_PARTITION",
      "_ROW_TIMESTAMP",
      "__ROOT__",
      "_COLIDENTIFIER",
      "_CHANGE_SEQUENCE_NUMBER",
      "_CHANGE_TYPE",
      "_CHANGE_TIMESTAMP",
  };
  return std::ranges::none_of(
             kReserved, [&upper](std::string_view prefix) { return upper.starts_with(prefix); }) &&
         !name.empty() && name.size() <= 300 &&
         (std::isalpha(static_cast<unsigned char>(name.front())) != 0 || name.front() == '_') &&
         std::ranges::all_of(name,
                             [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; });
}

// `name` in lower case, since BigQuery compares column names without case.
std::string Folded(std::string_view name) {
  std::string folded;
  std::ranges::transform(name, std::back_inserter(folded),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return folded;
}

// The field BigQuery detects for `column` of `metadata`: LIST inference is already applied by
// ParquetColumns. A group is a RECORD, a MAP the RECORD of its repeated key_value group, a repeated
// field REPEATED, and another field REQUIRED or NULLABLE as the file declares it. None for a field
// of a type or name the emulator does not detect.
std::optional<FieldSchema> DetectedField(const ThriftValue& metadata, const ParquetColumn& column) {
  if (!StandardColumnName(column.name)) {
    return std::nullopt;
  }
  FieldSchema field{.name = column.name};
  const ParquetColumn* value = &column;
  if (column.kind == ParquetColumn::Kind::kList) {
    field.mode = FieldMode::kRepeated;
    value = &column.children.front();
  } else {
    const ThriftValue* repetition = Element(metadata, column).Field(parquet::kElementRepetition);
    field.mode = repetition != nullptr && repetition->integer == parquet::kRequired
                     ? FieldMode::kRequired
                     : FieldMode::kNullable;
  }
  if (value->kind == ParquetColumn::Kind::kStruct || value->kind == ParquetColumn::Kind::kMap) {
    field.type = FieldType::kRecord;
    std::set<std::string> names;
    for (const ParquetColumn& child : value->children) {
      auto detected = DetectedField(metadata, child);
      if (!detected || !names.insert(Folded(child.name)).second) {
        return std::nullopt;
      }
      field.fields.push_back(*std::move(detected));
    }
    return field;
  }
  const auto type = DetectedType(Element(metadata, *value), *value);
  if (!type) {
    return std::nullopt;
  }
  field.type = *type;
  return field;
}

// The name BigQuery's messages give the physical type of `column`.
std::string_view PhysicalName(const ParquetColumn& column) {
  switch (column.type) {
    case parquet::kBoolean:
      return "BOOLEAN";
    case parquet::kInt32:
      return "INT32";
    case parquet::kInt64:
      return "INT64";
    case parquet::kInt96:
      return "INT96";
    case parquet::kFloat:
      return "FLOAT";
    case parquet::kDouble:
      return "DOUBLE";
    case parquet::kByteArray:
      return "BYTE_ARRAY";
    default:
      return "FIXED_LEN_BYTE_ARRAY";
  }
}

// `text` as a SQL literal in a conversion, which std::vformat later formats.
std::string ConversionLiteral(std::string_view text) {
  std::string escaped;
  for (const char c : QuoteLiteral(text)) {
    escaped += c;
    if (c == '{' || c == '}') {
      escaped += c;
    }
  }
  return escaped;
}

// Whether the integer `column` is signed: unannotated, or of a converted type INT_8 to INT_64.
bool SignedInteger(const ParquetColumn& column) {
  return column.IsInteger() &&
         (column.converted == -1 ? column.logical == 0
                                 : column.converted >= parquet::kConvertedFirstSignedInteger);
}

// The conversion, as ParquetConversion gives it, of the DECIMAL `column` of `uri` that NUMERIC
// does not hold, which BigQuery detects as NUMERIC, as `field` declares it, through a BIGNUMERIC
// of its bytes where StageParquetSources makes the reader read them, or none where
// ParquetConversion gives it. As BigQuery's answers in
// external_table_parquet_types.api.answers.json and external_table_types_open.api.answers.json
// show, it reads a value NUMERIC does not hold with an error naming its bytes, and it reads a
// STRING only of BIGNUMERIC's scale. Which bytes the error names of an INT32 or INT64 is not
// known, nor what it reads beyond BIGNUMERIC's range.
std::optional<std::string> UnheldDecimal(const FieldSchema& field, const ParquetColumn& column,
                                         std::string_view uri) {
  if (column.scale < 0 || column.precision - column.scale > 38) {
    throw ApiError::Unsupported(std::format("reading an external Parquet DECIMAL({}, {}) column",
                                            column.precision, column.scale));
  }
  const bool bytes = column.IsWideDecimal() || column.type == parquet::kFixedLenByteArray ||
                     column.type == parquet::kByteArray;
  const std::string units =
      bytes ? std::format("bq_bignumeric_from_decimal_bytes({{0}}, {})", column.scale)
            : "bq_bignumeric_from_string(CAST({0} AS VARCHAR), false)";
  switch (field.type) {
    case FieldType::kNumeric: {
      const auto fails = [&](std::string_view detail) {
        if (!bytes) {
          return "error(" +
                 ConversionLiteral(std::format("{}reading external Parquet column {} value ",
                                               kUnsupported, field.name)) +
                 " || bq_bignumeric_to_string(" + units + ") || " +
                 ConversionLiteral(", which NUMERIC does not hold, as NUMERIC") + ")";
        }
        return "error(" +
               ConversionLiteral(std::format("Error while reading data, error message: The value "
                                             "for column '{}' is out of valid NUMERIC range: {}; "
                                             "input length: ",
                                             field.name, detail)) +
               " || octet_length({0}) || " +
               ConversionLiteral(std::format("; scale: {}; input data: ", column.scale)) +
               " || lower(hex({0})) || " + ConversionLiteral(std::format(" File: {}", uri)) + ")";
      };
      const std::string fraction = "bq_bignumeric_round_digits(" + units + ", 9) = " + units;
      const std::string range = "bq_bignumeric_to_numeric(bq_bignumeric_trunc_digits(" + units +
                                ", 9), true) IS NOT NULL";
      // Which error BigQuery gives a value of both is not known.
      return "CASE WHEN {0} IS NULL THEN NULL WHEN NOT (" + range + ") AND NOT (" + fraction +
             ") THEN error(" +
             ConversionLiteral(std::format("{}reading external Parquet column {} of a value "
                                           "NUMERIC holds neither the integer part nor the "
                                           "fraction of",
                                           kUnsupported, field.name)) +
             ") WHEN NOT (" + range + ") THEN " +
             fails("Value is out of range after scaling to NUMERIC type") + " WHEN NOT (" +
             fraction + ") THEN " +
             fails("Value will lose precision after scaling down to NUMERIC type") +
             " ELSE bq_bignumeric_to_numeric(" + units + ", false) END";
    }
    case FieldType::kBigNumeric:
      return bytes ? std::optional(units) : std::nullopt;
    case FieldType::kString:
      if (column.scale > 38) {
        throw ApiError::Invalid(std::format(
            "Error while reading data, error message: Decimal type exceeds the valid range for "
            "BIGNUMERIC. Precision: {}, scale: {} File: {}",
            column.precision, column.scale, uri));
      }
      return bytes ? std::optional("bq_bignumeric_to_string(" + units + ")") : std::nullopt;
    default:
      return std::nullopt;
  }
}

// The DuckDB value, with {} for the column, of the top-level Parquet leaf `column`, of `element`,
// which BigQuery detects as `detected`, as the scalar `field` declares it, or empty where the
// reader's cast gives it. Throws BigQuery's error for a type it does not read the column as, and
// ApiError::Unsupported for other combinations. As BigQuery's answers in
// external_table_parquet_types.api.answers.json show, it reads an integer or a DECIMAL as NUMERIC
// or BIGNUMERIC, a DECIMAL as STRING and, by its unscaled value, as INT64, a BYTE_ARRAY as STRING
// or BYTES, a TIMESTAMP of milliseconds or microseconds as DATETIME, and a DATE, and a TIMESTAMP of
// nanoseconds, by their INT32 or INT64 value, as microseconds of a TIMESTAMP; and, as its answers
// in external_table_types_followups.api.answers.json show, a STRING or BYTE_ARRAY as GEOGRAPHY.
std::string ParquetConversion(const FieldSchema& field, const ThriftValue& element,
                              const ParquetColumn& column, FieldType detected,
                              std::string_view uri) {
  const bool nanos =
      column.type == parquet::kInt64 && Logical(element).first == parquet::kLogicalTimestamp &&
      TimeUnit(element, column) == parquet::kTimeUnitNanos && !AdjustedToUtc(element);
  const std::string_view physical = PhysicalName(column);
  const auto failed = [&](const std::string& detail) {
    return ApiError::Invalid(
        std::format("Error while reading data, error message: {} File: {}", detail, uri));
  };
  const auto unsupported = [&] {
    return ApiError::Unsupported(std::format("reading external Parquet column {} as {}", field.name,
                                             FieldTypeName(field.type)));
  };
  if (column.IsDecimal() && !column.HoldsNumeric()) {
    if (const auto conversion = UnheldDecimal(field, column, uri)) {
      return *conversion;
    }
  }
  if (field.type == detected) {
    return nanos ? "epoch_ns({})" : "";
  }
  const bool bytes =
      column.type == parquet::kByteArray || column.type == parquet::kFixedLenByteArray;
  switch (field.type) {
    case FieldType::kJson:
      throw ApiError::Invalid("Unsupported field type: JSON");
    case FieldType::kString:
      if (column.IsDecimal()) {
        // As NUMERIC text, without trailing fractional zeros.
        return R"(regexp_replace(regexp_replace(CAST({} AS VARCHAR), '(\.[0-9]*?)0+$', '\1'), '\.$', ''))";
      }
      if (detected == FieldType::kBytes) {
        return "decode({})";
      }
      if (!bytes) {
        throw failed(
            std::format("Parquet type for field '{}' needs to be FIXED_LEN_BYTE_ARRAY or "
                        "BYTE_ARRAY (found {}).",
                        field.name, physical));
      }
      break;
    case FieldType::kBytes:
      if (detected == FieldType::kString) {
        return "encode({})";
      }
      break;
    case FieldType::kFloat:
    case FieldType::kInteger:
      if (field.type == FieldType::kInteger && column.IsDecimal() &&
          (column.type == parquet::kInt32 || column.type == parquet::kInt64)) {
        return std::format("CAST({{}} * 1{} AS BIGINT)", std::string(column.scale, '0'));
      }
      if ((field.type == FieldType::kFloat && column.type != parquet::kFloat &&
           column.type != parquet::kDouble &&
           (detected == FieldType::kInteger || detected == FieldType::kNumeric)) ||
          (field.type == FieldType::kInteger &&
           (column.type == parquet::kFloat || column.type == parquet::kDouble ||
            column.type == parquet::kBoolean || column.type == parquet::kByteArray))) {
        throw ApiError::InvalidQuery(std::format(
            "Column '{}' has unsupported Parquet type({}) for GoogleSQL type({})", field.name,
            physical, field.type == FieldType::kFloat ? "FLOAT64" : "INT64"));
      }
      break;
    case FieldType::kNumeric:
    case FieldType::kBigNumeric:
      if (detected == FieldType::kInteger && !nanos) {
        return "";
      }
      if (column.IsDecimal()) {
        return "";
      }
      if (column.type == parquet::kFloat || column.type == parquet::kDouble ||
          detected == FieldType::kString) {
        throw failed(std::format(
            "Reading column '{}' of type {} as NUMERIC type but its logical type is {}, expecting "
            "DECIMAL.",
            field.name, physical, detected == FieldType::kString ? "UTF8" : "NONE"));
      }
      break;
    case FieldType::kDate:
      if (column.type == parquet::kInt64 || column.type == parquet::kByteArray) {
        // The physical type, numbered from 1.
        throw failed(std::format("Date type ({}) is not int32", column.type + 1));
      }
      break;
    case FieldType::kDatetime:
      if (detected == FieldType::kTimestamp && column.type == parquet::kInt64) {
        return "";
      }
      if (column.type == parquet::kInt32 || column.type == parquet::kByteArray) {
        throw failed(std::format("Datetime type ({}) is not INT64", physical));
      }
      if (nanos) {
        throw failed(std::format(
            "Datetime type column ({}) with type ({}) is not annotated as a TIMESTAMP, please "
            "annotate it as TIMESTAMP_MILLIS or TIMESTAMP_MICROS.",
            field.name, column.type + 1));
      }
      break;
    case FieldType::kGeography:
      // The reader reads WKT from a STRING and WKB from other bytes, as BigQuery's answers in
      // external_table_types_followups.api.answers.json show.
      if (detected == FieldType::kString ||
          (detected == FieldType::kBytes && column.type == parquet::kByteArray)) {
        return "";
      }
      break;
    case FieldType::kTimestamp:
      // As BigQuery's answers in external_table_parquet_types.api.answers.json and
      // external_table_types_open.api.answers.json show, it reads a BYTE_ARRAY as the TIMESTAMP
      // 0, a BOOLEAN or a signed integer by its value as microseconds, and a DOUBLE by its bits.
      if (column.type == parquet::kByteArray &&
          (detected == FieldType::kString || detected == FieldType::kBytes)) {
        return "CASE WHEN {0} IS NULL THEN NULL ELSE make_timestamp(0) END";
      }
      if (detected == FieldType::kDate || nanos || column.type == parquet::kBoolean ||
          column.type == parquet::kDouble ||
          (detected == FieldType::kInteger && SignedInteger(column))) {
        std::string micros = "CAST({0} AS BIGINT)";
        if (nanos) {
          micros = "epoch_ns({0})";
        } else if (detected == FieldType::kDate) {
          micros = "CAST({0} - DATE '1970-01-01' AS BIGINT)";
        } else if (column.type == parquet::kDouble) {
          micros = "CAST(CAST({0} AS BIT) AS BIGINT)";
        }
        return std::format(
            "CASE WHEN {0} BETWEEN -62135596800000000 AND 253402300799999999 THEN "
            "make_timestamp({0}) ELSE error('Cannot return an invalid timestamp value of ' || {0} "
            "|| ' microseconds relative to the Unix epoch. The range of valid timestamp values is "
            "[0001-01-01 00:00:00, 9999-12-31 23:59:59.999999]; error in writing field {1}') END",
            micros, std::string(field.name));
      }
      break;
    default:
      break;
  }
  throw unsupported();
}

// Whether a source whose column has the `detected` field, or none, can be read as `declared`:
// the same type, REPEATED in both or neither, and each declared field one of the source's, or
// NULLABLE and absent. BigQuery, as DuckDB's casts do, matches a RECORD's fields by name in any
// case and drops the source's other fields.
bool Matches(const FieldSchema& declared, const FieldSchema* detected) {
  if (detected == nullptr) {
    return declared.mode == FieldMode::kNullable;
  }
  if (declared.type != detected->type ||
      (declared.mode == FieldMode::kRepeated) != (detected->mode == FieldMode::kRepeated)) {
    return false;
  }
  return std::ranges::all_of(declared.fields, [detected](const FieldSchema& field) {
    const auto found = std::ranges::find_if(detected->fields, [&field](const FieldSchema& source) {
      return Folded(source.name) == Folded(field.name);
    });
    return Matches(field, found == detected->fields.end() ? nullptr : &*found);
  });
}

// `field` with each REQUIRED column and field in it NULLABLE: BigQuery reads a NULL value of an
// external table's REQUIRED column or field as NULL rather than failing.
FieldSchema Unenforced(FieldSchema field) {
  if (field.mode == FieldMode::kRequired) {
    field.mode = FieldMode::kNullable;
  }
  for (FieldSchema& child : field.fields) {
    child = Unenforced(std::move(child));
  }
  return field;
}

// Reads the footer of each source, rejecting GeoParquet, whose geometry BigQuery reads as
// GEOGRAPHY rather than the BYTES its columns hold.
ThriftValue SourceMetadata(const FileSource& source) {
  ThriftValue metadata = ReadParquetMetadata(source.path);
  if (HasGeoParquet(metadata)) {
    throw ApiError::Unsupported("GeoParquet external tables");
  }
  return metadata;
}

// The skipLeadingRows DetectCsv takes, absent when the table leaves it out.
std::optional<int> CsvSkipLeadingRows(const GcsExternalData& definition) {
  if (!definition.skip_leading_rows) {
    return std::nullopt;
  }
  return static_cast<int>(*definition.skip_leading_rows);
}

// The reader settings of a CSV table's options other than its delimiter and leading rows.
CsvReadOptions CsvOptions(const GcsExternalData& definition) {
  return {
      .quote =
          ParseCsvQuote(definition.quote.value_or("\""), definition.field_delimiter.value_or(",")),
      .allow_quoted_newlines = definition.allow_quoted_newlines.value_or(false),
      .allow_jagged_rows = definition.allow_jagged_rows.value_or(false),
      .ignore_unknown_values = definition.ignore_unknown_values.value_or(false),
      .null_marker = definition.null_marker,
      .null_markers = definition.null_markers,
      .preserve_ascii_control_characters =
          definition.preserve_ascii_control_characters.value_or(false),
      .external_values = true,
      .formats = ExternalFormats(definition),
  };
}

FileFormat Format(const GcsExternalData& definition) {
  if (definition.format == "CSV") {
    return FileFormat::kCsv;
  }
  return definition.format == "NEWLINE_DELIMITED_JSON" ? FileFormat::kJson : FileFormat::kParquet;
}

// Downloads the sources of `definition`, the external table `table_name`, and decodes those of a
// CSV or JSON table from its encoding. As BigQuery's answers in
// external_table_read_options.api.answers.json show, compression GZIP fails on a source that is
// not gzip. BigQuery reads a gzip source with compression NONE as it is, and splits one in UTF-16
// or UTF-32 into values before it decodes them, which both fail on gzip data; the emulator, which
// reads gzip whatever the compression, rejects them as unsupported.
std::vector<FileSource> StageSources(const GcsExternalData& definition, std::string_view table_name,
                                     GcsClient& client, TemporaryFiles& files) {
  const FileFormat format = Format(definition);
  std::vector<FileSource> sources = StageFileSources(definition.uris, format, client, files);
  if (format == FileFormat::kParquet) {
    return sources;
  }
  const TextEncoding encoding =
      ParseTextEncoding(definition.encoding.value_or("UTF-8")).value_or(TextEncoding::kUtf8);
  for (FileSource& source : sources) {
    // StageFileSources gives a gzip source the suffix .gz.
    const bool gzip = source.path.ends_with(".gz");
    if (definition.compression == "GZIP" && !gzip) {
      throw ApiError::Invalid(std::format(
          "Error while reading table: {}, error message: Decompression failed: Invalid gzip "
          "file: unrecognized format. File: {}",
          table_name, source.uri));
    }
    if (gzip && definition.compression == "NONE") {
      throw ApiError::Unsupported("reading a gzip external source with compression NONE: " +
                                  source.uri);
    }
    if (gzip && encoding != TextEncoding::kUtf8 && encoding != TextEncoding::kIso88591) {
      throw ApiError::Unsupported("reading a gzip external source in " +
                                  std::string(TextEncodingName(encoding)) + ": " + source.uri);
    }
    if (encoding != TextEncoding::kUtf8) {
      source.path = files.Write(DecodeText(ReadMaybeGzip(source.path), encoding));
    }
  }
  return sources;
}

// Adds to `bad` the records of the CSV `sources`, which StageCsvSources numbered with `bad`, that
// have a value of one of the `read` columns of `schema` the reader cannot convert, naming the
// first such column. `first` gives the number of the first record of each source.
void AddUnparsable(const std::vector<FileSource>& sources, const std::vector<int64_t>& first,
                   const CsvReadOptions& options, const std::vector<FieldSchema>& schema,
                   const std::vector<bool>& read, const TextQuery& query, BadRecords& bad) {
  std::vector<FieldSchema> text = schema;
  for (FieldSchema& field : text) {
    field.type = FieldType::kString;
  }
  const std::string number = QuoteIdentifier("bad.record");
  // The records a read of each column alone keeps. The reader converts only the values a query
  // returns, so these return them.
  std::vector<std::string> reads;
  for (size_t column = 0; column < schema.size(); ++column) {
    if (!read.at(column) || schema.at(column).type == FieldType::kString) {
      continue;
    }
    std::vector<FieldSchema> typed = text;
    typed.at(column) = schema.at(column);
    reads.push_back(std::format("SELECT '{}', CAST({} AS VARCHAR), CAST({} AS VARCHAR) FROM ({})",
                                column, number, QuoteIdentifier(schema.at(column).name),
                                FileReadQuery(options, sources, typed)));
  }
  if (reads.empty()) {
    return;
  }
  std::set<int64_t> skipped;
  std::ranges::transform(bad.found, std::inserter(skipped, skipped.end()), &BadRecord::number);
  std::map<size_t, std::set<int64_t>> kept;
  for (const auto& row : query(Join(reads, " UNION ALL "))) {
    kept[std::stoul(row.at(0).value_or(""))].insert(std::stoll(row.at(1).value_or("")));
  }
  // The first column of each record that a read of it leaves out.
  std::map<int64_t, size_t> unparsable;
  for (size_t column = 0; column < schema.size(); ++column) {
    if (!read.at(column) || schema.at(column).type == FieldType::kString) {
      continue;
    }
    for (int64_t record = 1; record <= bad.records; ++record) {
      if (!skipped.contains(record) && !kept[column].contains(record)) {
        unparsable.try_emplace(record, column);
      }
    }
  }
  if (unparsable.empty()) {
    return;
  }
  std::map<size_t, std::vector<std::string>> records;
  for (const auto& [record, column] : unparsable) {
    records[column].push_back(std::to_string(record));
  }
  std::vector<std::string> values;
  values.reserve(records.size());
  for (const auto& [column, numbers] : records) {
    values.push_back(
        std::format("SELECT CAST({0} AS VARCHAR), CAST({1} AS VARCHAR) FROM ({2}) "
                    "WHERE {0} IN ({3})",
                    number, QuoteIdentifier(schema.at(column).name),
                    FileReadQuery(options, sources, text), Join(numbers, ", ")));
  }
  for (const auto& row : query(Join(values, " UNION ALL "))) {
    const int64_t record = std::stoll(row.at(0).value_or(""));
    const size_t column = unparsable.at(record);
    const auto source = std::ranges::upper_bound(first, record) - first.begin() - 1;
    bad.found.push_back({
        .number = record,
        .uri = sources.at(source).uri,
        .message = CsvRecordError("Unable to parse", bad.positions.at(record - 1),
                                  &schema.at(column), column, row.at(1)),
        .external_conversion = schema.at(column).type == FieldType::kBytes ||
                               schema.at(column).type == FieldType::kBigNumeric ||
                               schema.at(column).type == FieldType::kInterval ||
                               schema.at(column).type == FieldType::kRange,
    });
  }
}

// `error`, of a CSV record of the external table `table_name` in the source `uri`, as a query
// fails with it: naming the table and the source, as BigQuery's answers in
// external_table_csv_types.api.answers.json and external_table_csv_autodetect.api.answers.json
// show.
ApiError RecordError(const ApiError& error, std::string_view table_name, std::string_view uri) {
  constexpr std::string_view kRecord = "Error while reading data, error message: ";
  const std::string_view message = error.what();
  if (!message.starts_with(kRecord)) {
    return error;
  }
  return error.WithMessage(std::format("Error while reading table: {}, error message: {} File: {}",
                                       table_name, message.substr(kRecord.size()), uri));
}

// Fails as BigQuery does when more records of the external table `table_name` than its limit are
// bad, and otherwise warns of each one in order. As BigQuery's answers in
// external_table_bad_records.api.answers.json show, it reads every source before it fails, and
// counts every bad record and every record but those of too many values; a JSON table of several
// files fails with the CSV message's form, after StageExternalJson has read all of them.
void ReportBadRecords(BadRecords& bad, bool csv, std::string_view table_name,
                      const TableReadContext& context) {
  std::ranges::sort(bad.found, {}, &BadRecord::number);
  // Native CSV reader errors name the first bad record without a limit. Several failures
  // of external conversions use the summary, as external_table_conversion_projection shows.
  if (csv && bad.limit == 0 && !bad.found.empty() &&
      (bad.found.size() == 1 || !bad.found.front().external_conversion)) {
    throw RecordError(ApiError::Invalid(bad.found.front().message), table_name,
                      bad.found.front().uri);
  }
  if (std::cmp_greater(bad.found.size(), bad.limit)) {
    throw ApiError::Invalid(std::format(
        "Error while reading table: {}, error message: {} processing encountered too many errors, "
        "giving up. Rows: {}; errors: {}; max bad: {}; error percent: 0",
        table_name, csv ? "CSV" : "JSON",
        bad.records -
            std::ranges::count_if(bad.found, [](const BadRecord& record) { return !record.row; }),
        bad.found.size(), bad.limit));
  }
  for (const BadRecord& record : bad.found) {
    if (record.query) {
      context.warn(ApiError::InvalidQuery(record.message));
      continue;
    }
    const std::string message =
        csv ? record.message
            : std::format("Error while reading table: {}, error message: {}", table_name,
                          record.message);
    context.warn(ApiError::Invalid(message + " File: " + record.uri).WithLocation(record.uri));
  }
}

}  // namespace

std::vector<FieldSchema> DetectGcsExternalSchema(const GcsExternalData& definition,
                                                 std::string_view table_id, GcsClient client) {
  const bool csv = definition.format == "CSV";
  const bool json = definition.format == "NEWLINE_DELIMITED_JSON";
  if (!csv && !json && definition.format != "PARQUET") {
    throw ApiError::Unsupported(definition.format + " external tables without an explicit schema");
  }
  if ((csv || json) && !definition.autodetect.value_or(false)) {
    throw ApiError::Invalid(
        "When defining a table with an ExternalDataConfiguration, a schema must be present on "
        "either the Table or the ExternalDataConfiguration. If the schema is present on both, the "
        "schemas must be the same.");
  }
  TemporaryFiles files;
  const auto sources = StageSources(definition, table_id, client, files);
  if (sources.empty()) {
    throw ApiError::Invalid("External table source URIs match no files");
  }
  // BigQuery takes the schema of the alphabetically last file, by its whole path.
  const FileSource& last = *std::ranges::max_element(sources, {}, &FileSource::uri);
  if (json) {
    return DetectJson(last, table_id, ExternalFormats(definition));
  }
  if (csv) {
    DetectedCsv detected =
        DetectCsv({last}, definition.field_delimiter.value_or(""), CsvSkipLeadingRows(definition),
                  files, /*rows=*/false, ExternalFormats(definition));
    if (detected.schema.empty()) {
      throw ApiError::Invalid("External CSV source " + last.uri + " has no columns");
    }
    return std::move(detected.schema);
  }
  const ThriftValue metadata = SourceMetadata(last);
  const ParquetColumn columns =
      ParquetColumns(metadata, definition.enable_list_inference.value_or(false));
  std::vector<FieldSchema> schema;
  std::set<std::string> names;
  for (const ParquetColumn& column : columns.children) {
    if (!StandardColumnName(column.name) || !names.insert(Folded(column.name)).second) {
      throw ApiError::Unsupported("detecting external table column name " + column.name);
    }
    auto field = DetectedField(metadata, column);
    // BigQuery detects a DECIMAL that NUMERIC does not hold as NUMERIC too, as its answers in
    // external_table_parquet_types.api.answers.json show.
    if (!field && StandardColumnName(column.name) && column.IsDecimal() &&
        column.edges == ParquetColumn::Edges::kNone) {
      const ThriftValue* repetition = Element(metadata, column).Field(parquet::kElementRepetition);
      field = FieldSchema{
          .name = column.name,
          .type = FieldType::kNumeric,
          .mode = repetition != nullptr && repetition->integer == parquet::kRequired
                      ? FieldMode::kRequired
                      : FieldMode::kNullable,
      };
    }
    if (!field) {
      throw ApiError::Unsupported("detecting the type of external Parquet column " + column.name);
    }
    schema.push_back(*std::move(field));
  }
  if (schema.empty()) {
    throw ApiError::Invalid("External Parquet source " + last.uri + " has no columns");
  }
  return schema;
}

std::vector<FieldSchema> DetectGcsPartitionKeys(const GcsExternalData& definition,
                                                const std::vector<FieldSchema>& declared,
                                                std::string_view table_id, GcsClient client) {
  if (!definition.hive_partitioning) {
    return {};
  }
  const HivePartitioning& hive = *definition.hive_partitioning;
  std::vector<FieldSchema> keys = declared;
  if (hive.mode == "CUSTOM" && keys.empty()) {
    keys = CustomPrefixKeys(hive.source_uri_prefix);
    if (keys.empty()) {
      throw ApiError::Invalid(std::format(
          "Prefix must contain a valid encoding of partition schema, with at least one partition "
          "key.  A proper example might be "
          "gs://bucket/table_name/{{dt:DATE}}/{{country:STRING}}.  Provided input: '{}'.",
          hive.source_uri_prefix));
    }
  }
  std::vector<std::string> uris;
  for (const std::string& uri : definition.uris) {
    std::ranges::copy(client.Expand(uri), std::back_inserter(uris));
  }
  return DetectHiveKeys(hive, keys, uris, table_id);
}

TableRead GcsExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                          std::string table_name, GcsClient client) {
  return [definition = std::move(definition), schema = std::move(schema),
          table_name = std::move(table_name),
          client = std::move(client)](TableReadContext& context) mutable {
    TemporaryFiles& files = context.files;
    const bool csv = definition.format == "CSV";
    const bool json = definition.format == "NEWLINE_DELIMITED_JSON";
    // tables.insert stores formats and time zones that a query then rejects.
    if (const absl::Status formats =
            CheckDatetimeFormats(ExternalFormats(definition), /*ddl=*/false);
        !formats.ok()) {
      throw ApiError::InvalidQuery(std::string(formats.message()));
    }
    if (context.dry_run) {
      // Even preparing a file reader can access GCS. Bind only the stored schema, including
      // the pseudocolumn which is not part of that schema, without listing or staging files.
      std::vector<std::string> columns;
      std::ranges::transform(schema, std::back_inserter(columns), [](const FieldSchema& field) {
        return "CAST(NULL AS " + ToDuckDbReadType(field) + ") AS " + QuoteIdentifier(field.name);
      });
      columns.emplace_back("CAST(NULL AS VARCHAR) AS _FILE_NAME");
      return "SELECT " + Join(columns, ", ") + " WHERE FALSE";
    }
    const auto sources = StageSources(definition, table_name, client, files);
    // BigQuery ignores maxBadRecords for Parquet. A JSON table of one file fails at the record
    // that exceeds the limit. A CSV read finds its bad records even without a limit, since the
    // reader cannot report a value it fails to convert as BigQuery does.
    BadRecords bad{
        .limit = definition.max_bad_records.value_or(0),
        .stops = json && sources.size() == 1,
    };
    BadRecords* const skips = csv || (bad.limit > 0 && json) ? &bad : nullptr;
    // A partitioned table reads its keys from each source's path rather than its contents.
    const HivePartitioning* hive =
        definition.hive_partitioning && !definition.hive_partitioning->mode.empty()
            ? &*definition.hive_partitioning
            : nullptr;
    std::vector<FieldSchema> keys;
    std::vector<FieldSchema> file_schema = schema;
    if (hive != nullptr) {
      std::erase_if(file_schema, [hive](const FieldSchema& field) {
        return std::ranges::find(hive->fields, field.name) != hive->fields.end();
      });
      std::ranges::transform(hive->fields, std::back_inserter(keys), [&schema](const auto& name) {
        return *std::ranges::find(schema, name, &FieldSchema::name);
      });
      // As when BigQuery finds a key that is also a column of the files.
      if (file_schema.empty()) {
        throw ApiError::Invalid(
            "Hive partitioning requires autodetect to be set or a non-empty schema to be "
            "provided.");
      }
    }
    const std::string table_id = table_name.substr(table_name.rfind('.') + 1);
    // The columns of a source's `read`, its keys and _FILE_NAME.
    const auto select = [&](const FileSource& source, const std::string& read) {
      std::string columns = "*";
      if (hive != nullptr) {
        const auto values = HiveValues(*hive, keys, source.uri, table_id);
        for (size_t i = 0; i < keys.size(); ++i) {
          columns += ", CAST(" + (values.at(i) ? QuoteLiteral(*values.at(i)) : "NULL") + " AS " +
                     ToDuckDbReadType(keys.at(i)) + ") AS " + QuoteIdentifier(keys.at(i).name);
        }
      }
      return "SELECT " + columns + ", " + QuoteLiteral(source.uri) + " AS _FILE_NAME FROM (" +
             read + ") AS source";
    };
    std::vector<FieldSchema> read_schema;
    std::ranges::transform(file_schema, std::back_inserter(read_schema), Unenforced);
    // A CSV table converts only the columns the statement reads, as BigQuery does, and reads the
    // others as NULL.
    std::vector<bool> read(read_schema.size(), true);
    std::vector<FieldSchema> converted = read_schema;
    std::vector<std::string> unread;
    if (csv && context.columns) {
      for (size_t i = 0; i < read_schema.size(); ++i) {
        if (!context.columns->contains(read_schema.at(i).name)) {
          read.at(i) = false;
          converted.at(i).type = FieldType::kString;
          unread.push_back(std::format("CAST(NULL AS {}) AS {}",
                                       ToDuckDbReadType(read_schema.at(i)),
                                       QuoteIdentifier(read_schema.at(i).name)));
        }
      }
    }
    std::vector<FileSource> staged_csv;
    std::vector<int64_t> first_records;
    // An autodetected table reads each file with the delimiter and header rows BigQuery detects
    // in the alphabetically last one, which gives its schema, and matches columns by position.
    std::string delimiter = definition.field_delimiter.value_or(",");
    int skip_leading_rows = static_cast<int>(definition.skip_leading_rows.value_or(0));
    if (csv && definition.autodetect.value_or(false) && !sources.empty()) {
      const DetectedCsv detected =
          DetectCsv({*std::ranges::max_element(sources, {}, &FileSource::uri)},
                    definition.field_delimiter.value_or(""), CsvSkipLeadingRows(definition), files,
                    /*rows=*/false, ExternalFormats(definition));
      delimiter = std::string(1, detected.delimiter);
      skip_leading_rows = detected.skipped_rows;
    }
    CsvReadOptions csv_options = CsvOptions(definition);
    csv_options.skip_leading_rows = skip_leading_rows;
    csv_options.field_delimiter = delimiter;
    csv_options.numbered_records = skips != nullptr;
    std::vector<std::string> reads;
    for (const auto& source : sources) {
      if (json) {
        std::vector<FileSource> staged;
        try {
          staged =
              StageExternalJson({source}, file_schema, files, definition.autodetect.value_or(false),
                                definition.given_schema, ExternalFormats(definition), skips);
        } catch (const ApiError& error) {
          if (std::string_view(error.what()).starts_with(kUnsupported) ||
              error.reason() == "invalidQuery") {
            throw;
          }
          throw error.WithMessage(std::format("Error while reading table: {}, error message: {}",
                                              table_name, error.what()));
        }
        reads.push_back(select(
            source, FileReadQuery(JsonReadOptions{.explicit_schema = true}, staged, read_schema)));
        continue;
      }
      if (csv) {
        first_records.push_back(bad.records + 1);
        std::vector<FileSource> staged;
        try {
          staged =
              StageCsvSources({source}, csv_options, read_schema, files, skips, context.columns);
        } catch (const ApiError& error) {
          // A record the parser cannot split fails the read whatever its limit.
          throw RecordError(error, table_name, source.uri);
        }
        std::ranges::copy(staged, std::back_inserter(staged_csv));
        std::string read_csv = FileReadQuery(csv_options, staged, converted);
        if (!unread.empty()) {
          read_csv = std::format("SELECT * REPLACE ({}) FROM ({})", Join(unread, ", "), read_csv);
        }
        reads.push_back(select(source, read_csv));
        continue;
      }
      const ThriftValue metadata = SourceMetadata(source);
      const auto columns =
          ParquetColumns(metadata, definition.enable_list_inference.value_or(false));
      // Do not inherit load's coercions: a top-level column reads as ParquetConversion gives
      // it, and a nested one must have the type BigQuery would detect for it, so that no
      // incompatible column is silently cast to the declared type.
      std::map<std::string, std::string> conversions;
      for (const auto& field : file_schema) {
        const auto* column = columns.Child(field.name);
        const auto detected = column == nullptr ? std::nullopt : DetectedField(metadata, *column);
        if (detected && detected->mode == FieldMode::kRepeated &&
            field.mode != FieldMode::kRepeated) {
          throw ApiError::Invalid(std::format(
              "Error while reading data, error message: Schema mismatch: field "
              "'Msg_0_CLOUD_QUERY_TABLE.{}' is repeated in file (max repetition level 1) but "
              "expected to be optional/required in BigQuery File: {}",
              field.name, source.uri));
        }
        // BigQuery detects a DECIMAL that NUMERIC does not hold as NUMERIC too.
        const bool decimal = column != nullptr && !detected && column->IsDecimal();
        if (column != nullptr && (detected || decimal) &&
            column->kind == ParquetColumn::Kind::kLeaf &&
            column->edges == ParquetColumn::Edges::kNone && field.mode != FieldMode::kRepeated &&
            field.type != FieldType::kRecord &&
            (decimal ||
             (detected->mode != FieldMode::kRepeated && detected->type != FieldType::kRecord))) {
          std::string conversion =
              ParquetConversion(field, Element(metadata, *column), *column,
                                decimal ? FieldType::kNumeric : detected->type, source.uri);
          if (!conversion.empty()) {
            conversions.emplace(field.name, std::move(conversion));
          }
          continue;
        }
        if ((column != nullptr && !detected) || !Matches(field, detected ? &*detected : nullptr)) {
          throw ApiError::Unsupported(std::string("external Parquet source schema for column ") +
                                      field.name);
        }
      }
      // Each source is read with its own columns, which put back what DuckDB unwraps. A copy
      // without the annotations of DECIMALs that NUMERIC does not hold gives their bytes, which
      // UnheldDecimal reads, and DuckDB reads a wide one as a DOUBLE otherwise.
      const bool decimal_bytes =
          columns.HasWideDecimal() ||
          std::ranges::any_of(columns.children, [](const ParquetColumn& column) {
            return column.IsDecimal() && !column.HoldsNumeric() &&
                   (column.type == parquet::kFixedLenByteArray ||
                    column.type == parquet::kByteArray);
          });
      const FileSource read_source =
          decimal_bytes ? StageParquetSources({source}, files,
                                              definition.enable_list_inference.value_or(false),
                                              /*decimal_bytes=*/true)
                              .sources.front()
                        : source;
      reads.push_back(select(source, FileReadQuery(
                                         ParquetReadOptions{
                                             .allow_missing_columns = true,
                                             .allow_null_list_elements = true,
                                             .conversions = std::move(conversions),
                                         },
                                         {read_source}, read_schema, columns)));
    }
    if (reads.empty()) {
      throw ApiError::Invalid("External table source URIs match no files");
    }
    if (skips == nullptr) {
      return Join(reads, " UNION ALL ");
    }
    // Of a JSON table, only the records StageExternalJson rejects count as bad; a value its
    // reader cannot convert still fails the read.
    if (csv) {
      AddUnparsable(staged_csv, first_records, csv_options, read_schema, read, context.query, bad);
    }
    ReportBadRecords(bad, csv, table_name, context);
    if (!csv) {
      return Join(reads, " UNION ALL ");
    }
    // The reader leaves out a record with a value it cannot convert only where a query uses that
    // value, so the read leaves out every bad record by its number.
    const std::string number = QuoteIdentifier("bad.record");
    std::string read_csv =
        std::format("SELECT * EXCLUDE ({}) FROM ({})", number, Join(reads, " UNION ALL "));
    if (!bad.found.empty()) {
      std::vector<std::string> numbers;
      std::ranges::transform(bad.found, std::back_inserter(numbers),
                             [](const BadRecord& record) { return std::to_string(record.number); });
      read_csv += std::format(" WHERE {} NOT IN ({})", number, Join(numbers, ", "));
    }
    return read_csv;
  };
}

}  // namespace bigquery_emulator_duckdb
