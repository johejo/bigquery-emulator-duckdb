#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "nlohmann/json_fwd.hpp"
#include "src/extract_options.h"
#include "src/file_read_options.h"

namespace bigquery_emulator_duckdb {

std::string_view FileFormatName(FileFormat format);
FileFormat ParseLoadFormat(const nlohmann::json& configuration);

struct CsvLoadOptions {
  CsvReadOptions read;
  // Whether the job sets an option other than skipLeadingRows and fieldDelimiter, which schema
  // auto-detection does not take.
  bool detection_options = false;
};
struct JsonLoadOptions {};
struct ParquetLoadOptions {
  bool enable_list_inference = false;
};
using LoadOptions = std::variant<CsvLoadOptions, JsonLoadOptions, ParquetLoadOptions>;
LoadOptions ParseLoadOptions(FileFormat format, const nlohmann::json& configuration);
// Throws ApiError::Invalid for the CSV options BigQuery rejects with jobs.insert rather than in
// the job: a quote of several characters, and nullMarker with nullMarkers.
void CheckLoadRequest(const nlohmann::json& configuration);
std::vector<std::string> ParseLoadSourceUris(const nlohmann::json& configuration);
// Decimal inference only consults known string entries; other entries are ignored.
std::vector<DecimalTargetType> ParseDecimalTargetTypes(const nlohmann::json& configuration);

// JSON is retained separately on the job for jobs.get; execution uses these types.
struct ExtractEncoding {
  FileFormat format = FileFormat::kCsv;
  ExtractCompression compression = ExtractCompression::kNone;
};
ExtractEncoding ParseExtractEncoding(const nlohmann::json& configuration);
// Text fields are decoded after reading the source, preserving job validation order.
TextExtractFormatOptions ParseTextExtractFormatOptions(FileFormat format,
                                                       const nlohmann::json& configuration);

}  // namespace bigquery_emulator_duckdb
