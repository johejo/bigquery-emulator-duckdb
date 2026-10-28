#include "src/external_table.h"

#include <string>
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

// Do not inherit load's coercions: external Parquet reads must not silently cast an
// incompatible source column to the declared type. Extend this deliberately with verified forms.
bool Matches(const ParquetColumn& column, FieldType type) {
  if (column.kind != ParquetColumn::Kind::kLeaf || column.edges != ParquetColumn::Edges::kNone) {
    return false;
  }
  switch (type) {
    case FieldType::kInteger:
      return column.type == parquet::kInt64 && column.converted == -1 && column.logical == 0;
    case FieldType::kBoolean:
      return column.type == parquet::kBoolean && column.converted == -1 && column.logical == 0;
    case FieldType::kFloat:
      return column.type == parquet::kDouble && column.converted == -1 && column.logical == 0;
    case FieldType::kString:
      return column.type == parquet::kByteArray && (column.converted == parquet::kConvertedUtf8 ||
                                                    column.logical == parquet::kLogicalString);
    case FieldType::kBytes:
      return column.type == parquet::kByteArray && column.converted == -1 && column.logical == 0;
    default:
      return false;
  }
}

}  // namespace

TableRead GcsExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                          GcsClient client) {
  return [definition = std::move(definition), schema = std::move(schema),
          client = std::move(client)](TemporaryFiles& files) mutable {
    const auto sources = StageFileSources(definition.uris, FileFormat::kParquet, client, files);
    for (const auto& source : sources) {
      const auto columns = ParquetColumns(ReadParquetMetadata(source.path), false);
      for (const auto& field : schema) {
        const auto* column = columns.Child(field.name);
        if (column == nullptr || !Matches(*column, field.type)) {
          throw ApiError::Unsupported("external Parquet source schema for column " + field.name);
        }
      }
    }
    return FileReadQuery(ParquetReadOptions{}, sources, schema);
  };
}

}  // namespace bigquery_emulator_duckdb
