#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace bigquery_emulator_duckdb {

// Shared by extract job configuration parsing and output writers.
enum class ExtractCompression : uint8_t { kNone, kGzip, kSnappy, kZstd };
std::string_view ExtractCompressionName(ExtractCompression compression);

struct CsvExtractOptions {
  std::string field_delimiter = ",";
  bool print_header = true;
};
struct JsonExtractOptions {};
using TextExtractFormatOptions = std::variant<CsvExtractOptions, JsonExtractOptions>;

}  // namespace bigquery_emulator_duckdb
