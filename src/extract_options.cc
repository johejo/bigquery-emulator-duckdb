#include "src/extract_options.h"

#include <string_view>

namespace bigquery_emulator_duckdb {

std::string_view ExtractCompressionName(ExtractCompression compression) {
  switch (compression) {
    case ExtractCompression::kNone:
      return "NONE";
    case ExtractCompression::kGzip:
      return "GZIP";
    case ExtractCompression::kSnappy:
      return "SNAPPY";
    case ExtractCompression::kZstd:
      return "ZSTD";
  }
  return "";
}

}  // namespace bigquery_emulator_duckdb
