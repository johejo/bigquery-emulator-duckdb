#include "src/job_options.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/extract_options.h"

namespace bigquery_emulator_duckdb {
namespace {
std::optional<FileFormat> ParseFormat(std::string_view name) {
  for (const auto format : {FileFormat::kCsv, FileFormat::kJson, FileFormat::kParquet}) {
    if (FileFormatName(format) == name) {
      return format;
    }
  }
  return std::nullopt;
}
}  // namespace

std::string_view FileFormatName(FileFormat format) {
  switch (format) {
    case FileFormat::kCsv:
      return "CSV";
    case FileFormat::kJson:
      return "NEWLINE_DELIMITED_JSON";
    case FileFormat::kParquet:
      return "PARQUET";
  }
  return "";
}
FileFormat ParseLoadFormat(const nlohmann::json& config) {
  const std::string name = config.value("sourceFormat", "CSV");
  const auto format = ParseFormat(name);
  if (!format) {
    throw ApiError::Invalid("Unsupported source format: " + name);
  }
  return *format;
}
LoadOptions ParseLoadOptions(FileFormat format, const nlohmann::json& config) {
  switch (format) {
    case FileFormat::kCsv:
      return CsvLoadOptions{
          .skip_leading_rows = config.value("skipLeadingRows", 0),
          .field_delimiter = config.value("fieldDelimiter", ","),
      };
    case FileFormat::kJson:
      return JsonLoadOptions{};
    case FileFormat::kParquet: {
      const auto options = config.value("parquetOptions", nlohmann::json::object());
      return ParquetLoadOptions{
          .enable_list_inference = options.value("enableListInference", false),
      };
    }
  }
  return JsonLoadOptions{};
}
std::vector<std::string> ParseLoadSourceUris(const nlohmann::json& config) {
  const auto uris = config.value("sourceUris", nlohmann::json::array());
  if (!uris.is_array() || uris.empty()) {
    throw ApiError::Invalid("sourceUris is required");
  }
  std::vector<std::string> result;
  for (const auto& uri : uris) {
    if (!uri.is_string()) {
      throw ApiError::Invalid("Invalid source URI");
    }
    result.push_back(uri.get<std::string>());
  }
  return result;
}
std::vector<DecimalTargetType> ParseDecimalTargetTypes(const nlohmann::json& config) {
  const auto targets = config.value("decimalTargetTypes", nlohmann::json::array());
  if (!targets.is_array()) {
    throw ApiError::Invalid("Invalid decimalTargetTypes");
  }
  if (targets.empty()) {
    return {DecimalTargetType::kNumeric};
  }
  std::vector<DecimalTargetType> result;
  for (const auto& target : targets) {
    if (target == "NUMERIC") {
      result.push_back(DecimalTargetType::kNumeric);
    }
    if (target == "BIGNUMERIC") {
      result.push_back(DecimalTargetType::kBigNumeric);
    }
    if (target == "STRING") {
      result.push_back(DecimalTargetType::kString);
    }
  }
  return result;
}
ExtractEncoding ParseExtractEncoding(const nlohmann::json& config) {
  const std::string name = config.value("destinationFormat", "CSV");
  const std::string compression = config.value("compression", "NONE");
  if (name == "AVRO") {
    throw ApiError::Unsupported("Avro extracts");
  }
  const auto format = ParseFormat(name);
  if (!format) {
    throw ApiError::Invalid("Unsupported destination format: " + name);
  }
  std::optional<ExtractCompression> parsed;
  for (const auto value : {
           ExtractCompression::kNone,
           ExtractCompression::kGzip,
           ExtractCompression::kSnappy,
           ExtractCompression::kZstd,
       }) {
    if (ExtractCompressionName(value) == compression &&
        (*format == FileFormat::kParquet || value == ExtractCompression::kNone ||
         value == ExtractCompression::kGzip)) {
      parsed = value;
    }
  }
  if (!parsed) {
    throw ApiError::Invalid("Unsupported compression " + compression + " for " + name);
  }
  return {.format = *format, .compression = *parsed};
}
TextExtractFormatOptions ParseTextExtractFormatOptions(FileFormat format,
                                                       const nlohmann::json& config) {
  switch (format) {
    case FileFormat::kCsv:
      return CsvExtractOptions{
          .field_delimiter = config.value("fieldDelimiter", ","),
          .print_header = config.value("printHeader", true),
      };
    case FileFormat::kJson:
      // These fields were read for all text extracts, even though JSON does not use them.
      (void)config.value("fieldDelimiter", ",");
      (void)config.value("printHeader", true);
      return JsonExtractOptions{};
    case FileFormat::kParquet:
      throw ApiError::Invalid("Parquet is not a text extract format");
  }
  return JsonExtractOptions{};
}
}  // namespace bigquery_emulator_duckdb
