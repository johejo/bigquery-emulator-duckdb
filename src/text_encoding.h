#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bigquery_emulator_duckdb {

// The character encodings of CSV and JSON sources.
enum class TextEncoding : uint8_t { kUtf8, kIso88591, kUtf16Be, kUtf16Le, kUtf32Be, kUtf32Le };

// The name tables.get gives `encoding`: UTF-8, ISO-8859-1, UTF-16BE, UTF-16LE, UTF-32BE or
// UTF-32LE.
std::string_view TextEncodingName(TextEncoding encoding);

// The encoding an encoding option names, none for another name. The REST API takes the names
// TextEncodingName gives; the `ddl` option takes them in any case, and UTF8 and ISO_8859_1.
std::optional<TextEncoding> ParseTextEncoding(std::string_view name, bool ddl = false);

// `bytes` in `encoding` as UTF-8, without a leading UTF-16 or UTF-32 byte order mark; UTF-8 is
// returned as it is. Throws ApiError::Unsupported for a sequence that is not a character, which
// BigQuery's answers do not settle.
std::string DecodeText(std::string_view bytes, TextEncoding encoding);

}  // namespace bigquery_emulator_duckdb
