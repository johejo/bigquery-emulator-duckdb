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
  int skip_leading_rows = 0;
  std::string field_delimiter = ",";
};
struct JsonLoadOptions {};
struct ParquetLoadOptions {
  bool enable_list_inference = false;
};
using LoadOptions = std::variant<CsvLoadOptions, JsonLoadOptions, ParquetLoadOptions>;
LoadOptions ParseLoadOptions(FileFormat format, const nlohmann::json& configuration);
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
