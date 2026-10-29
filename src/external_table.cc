#include "src/external_table.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/api_error.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/file_read_options.h"
#include "src/gcs.h"
#include "src/parquet_metadata.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"
#include "src/temporary_files.h"

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

// The type BigQuery detects for the Parquet leaf `column`, whose SchemaElement is `element`, as
// the Parquet conversions of
// [Loading Parquet data](third_party/bigquery_docs/loading-data-cloud-storage-parquet.md)
// list them, or none for other combinations, which BigQuery does not convert, and for ENUM and
// GeoParquet, whose types depend on options the emulator does not take. A DECIMAL takes the
// default decimalTargetTypes, NUMERIC, which reads one NUMERIC cannot hold with an error only when
// a value exceeds its range; the emulator does not take such a DECIMAL.
std::optional<FieldType> DetectedType(const ThriftValue& element, const ParquetColumn& column) {
  if (column.kind != ParquetColumn::Kind::kLeaf || column.edges != ParquetColumn::Edges::kNone) {
    return std::nullopt;
  }
  if (column.IsDecimal()) {
    const bool physical = column.type == parquet::kInt32 || column.type == parquet::kInt64 ||
                          column.type == parquet::kFixedLenByteArray;
    if (physical && column.scale >= 0 && column.scale <= 9 &&
        column.precision - column.scale <= 29) {
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

// The mode of the column whose SchemaElement is `element`: a REPEATED field is a list, which the
// emulator does not take.
FieldMode DetectedMode(const ThriftValue& element) {
  const ThriftValue* repetition = element.Field(parquet::kElementRepetition);
  return repetition != nullptr && repetition->integer == parquet::kRequired ? FieldMode::kRequired
                                                                            : FieldMode::kNullable;
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

// Reads the footer of each source, rejecting GeoParquet, whose geometry BigQuery reads as
// GEOGRAPHY rather than the BYTES its columns hold.
ThriftValue SourceMetadata(const FileSource& source) {
  ThriftValue metadata = ReadParquetMetadata(source.path);
  if (HasGeoParquet(metadata)) {
    throw ApiError::Unsupported("GeoParquet external tables");
  }
  return metadata;
}

}  // namespace

std::vector<FieldSchema> DetectGcsExternalSchema(const GcsExternalData& definition,
                                                 GcsClient client) {
  TemporaryFiles files;
  const auto sources = StageFileSources(definition.uris, FileFormat::kParquet, client, files);
  if (sources.empty()) {
    throw ApiError::Invalid("External table source URIs match no files");
  }
  // BigQuery takes the schema of the alphabetically last file, by its whole path.
  const FileSource& last = *std::ranges::max_element(sources, {}, &FileSource::uri);
  const ThriftValue metadata = SourceMetadata(last);
  const ParquetColumn columns = ParquetColumns(metadata, false);
  std::vector<FieldSchema> schema;
  std::set<std::string> names;
  for (const ParquetColumn& column : columns.children) {
    std::string folded;
    std::ranges::transform(column.name, std::back_inserter(folded),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!StandardColumnName(column.name) || !names.insert(folded).second) {
      throw ApiError::Unsupported("detecting external table column name " + column.name);
    }
    const ThriftValue& element = Element(metadata, column);
    const auto type = DetectedType(element, column);
    if (!type) {
      throw ApiError::Unsupported("detecting the type of external Parquet column " + column.name);
    }
    schema.push_back(
        FieldSchema{.name = column.name, .type = *type, .mode = DetectedMode(element)});
  }
  if (schema.empty()) {
    throw ApiError::Invalid("External Parquet source " + last.uri + " has no columns");
  }
  return schema;
}

TableRead GcsExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                          GcsClient client) {
  return [definition = std::move(definition), schema = std::move(schema),
          client = std::move(client)](TemporaryFiles& files) mutable {
    const auto sources = StageFileSources(definition.uris, FileFormat::kParquet, client, files);
    for (const auto& source : sources) {
      const ThriftValue metadata = SourceMetadata(source);
      const auto columns = ParquetColumns(metadata, false);
      // Do not inherit load's coercions: a source column must have the type BigQuery would
      // detect for it, so that no incompatible column is silently cast to the declared type.
      for (const auto& field : schema) {
        const auto* column = columns.Child(field.name);
        if (column == nullptr || DetectedType(Element(metadata, *column), *column) != field.type ||
            (field.mode == FieldMode::kRequired &&
             DetectedMode(Element(metadata, *column)) != FieldMode::kRequired)) {
          throw ApiError::Unsupported(std::string("external Parquet source schema for column ") +
                                      field.name);
        }
      }
    }
    return FileReadQuery(ParquetReadOptions{}, sources, schema);
  };
}

}  // namespace bigquery_emulator_duckdb
