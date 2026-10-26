#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "nlohmann/json_fwd.hpp"

namespace bigquery_emulator_duckdb {

enum class FileFormat : uint8_t { kCsv, kJson, kParquet };
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
enum class DecimalTargetType : uint8_t { kNumeric, kBigNumeric, kString };
// Decimal inference only consults known string entries; other entries are ignored.
std::vector<DecimalTargetType> ParseDecimalTargetTypes(const nlohmann::json& configuration);

// JSON is retained separately on the job for jobs.get; execution uses these types.
enum class ExtractCompression : uint8_t { kNone, kGzip, kSnappy, kZstd };
std::string_view ExtractCompressionName(ExtractCompression compression);
struct CsvExtractOptions {
  std::string field_delimiter = ",";
  bool print_header = true;
};
struct JsonExtractOptions {};
struct ParquetExtractOptions {};
struct ExtractEncoding {
  FileFormat format = FileFormat::kCsv;
  ExtractCompression compression = ExtractCompression::kNone;
};
ExtractEncoding ParseExtractEncoding(const nlohmann::json& configuration);
using ExtractFormatOptions =
    std::variant<CsvExtractOptions, JsonExtractOptions, ParquetExtractOptions>;
// Text fields are decoded after reading the source, preserving job validation order.
ExtractFormatOptions ParseExtractFormatOptions(FileFormat format,
                                               const nlohmann::json& configuration);

}  // namespace bigquery_emulator_duckdb
