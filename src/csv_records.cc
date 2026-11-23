#include "src/csv_records.h"

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/common/utf_util.h"
#include "src/api_error.h"
#include "src/text_encoding.h"

namespace bigquery_emulator_duckdb {

std::vector<CsvRecord> ParseCsvRecords(std::string_view contents, const CsvDialect& dialect,
                                       size_t limit, bool lenient) {
  std::vector<CsvRecord> records;
  size_t line = 1;
  size_t i = 0;
  const auto line_break = [&] { return contents.at(i) == '\n' || contents.at(i) == '\r'; };
  const auto line_end = [&] { return i >= contents.size() || line_break(); };
  while (i < contents.size() && records.size() < limit) {
    CsvRecord record{.cells = {}, .line = line, .offset = i};
    while (true) {
      CsvCell cell;
      const auto decode = [&] {
        if (!dialect.cell_encoding) {
          return;
        }
        if (*dialect.cell_encoding == TextEncoding::kUtf8) {
          // Splitting at a multibyte separator's first byte can leave invalid UTF-8 in a field.
          cell.text = googlesql::CoerceToWellFormedUTF8(cell.text);
        } else {
          cell.text = DecodeText(cell.text, *dialect.cell_encoding);
        }
      };
      if (dialect.quote && i < contents.size() && contents.at(i) == *dialect.quote) {
        cell.quoted = true;
        for (++i;; ++i) {
          if (i >= contents.size() || (!dialect.allow_quoted_newlines && line_break())) {
            if (lenient) {
              break;
            }
            record.error = std::format("Missing close quote character ({}).", *dialect.quote);
            break;
          }
          if (contents.at(i) == *dialect.quote) {
            if (i + 1 >= contents.size() || contents.at(i + 1) != *dialect.quote) {
              ++i;
              break;
            }
            ++i;
          } else if (contents.at(i) == '\r' && i + 1 < contents.size() &&
                     contents.at(i + 1) == '\n') {
            // A quoted CRLF reads as LF.
            continue;
          } else if (contents.at(i) == '\n') {
            ++line;
          }
          cell.text += contents.at(i);
        }
        if (!lenient && !record.error && !line_end() && contents.at(i) != dialect.delimiter) {
          record.error = std::format("Data between close quote character ({}) and field separator.",
                                     *dialect.quote);
        }
        if (record.error) {
          decode();
          record.cells.emplace_back(std::move(cell));
          records.push_back(std::move(record));
          return records;
        }
      }
      for (; !line_end() && contents.at(i) != dialect.delimiter; ++i) {
        cell.text += contents.at(i);
      }
      decode();
      record.cells.push_back(cell.quoted || !cell.text.empty() ? std::optional(std::move(cell))
                                                               : std::nullopt);
      if (line_end()) {
        break;
      }
      ++i;
    }
    if (i < contents.size() && contents.at(i) == '\r') {
      ++i;
    }
    if (i < contents.size() && contents.at(i) == '\n') {
      ++i;
    }
    ++line;
    if (record.cells.size() > 1 || record.cells.front().has_value()) {
      records.push_back(std::move(record));
    }
  }
  return records;
}

char ExternalCsvDelimiter(std::string_view option, TextEncoding encoding) {
  const char delimiter = option.empty() ? '\0' : option.front();
  const auto lead = static_cast<unsigned char>(delimiter);
  if (delimiter != '\0' && delimiter != '\n' && delimiter != '\r' && delimiter != '"') {
    if (lead < 0x80 || encoding == TextEncoding::kUtf8) {
      return delimiter;
    }
    // U+0080 through U+00FF are the two UTF-8 sequences starting with C2 and C3.
    if (encoding == TextEncoding::kIso88591 && (lead == 0xc2 || lead == 0xc3) &&
        option.size() >= 2) {
      const auto tail = static_cast<unsigned char>(option.at(1));
      if (tail >= 0x80 && tail <= 0xbf) {
        return static_cast<char>(((lead & 0x03U) << 6U) | (tail & 0x3fU));
      }
    }
  }
  throw ApiError::Unsupported("CSV external table field_delimiter");
}

std::optional<char> ParseCsvQuote(const std::string& quote, const std::string& field_delimiter) {
  if (quote.empty()) {
    return std::nullopt;
  }
  if (quote.size() > 1) {
    throw ApiError::Invalid("Quote char must be at most a single character.");
  }
  const auto c = static_cast<unsigned char>(quote.front());
  if (c > 127 || c == '\n' || c == '\r' || c == '\0' || quote == field_delimiter) {
    throw ApiError::Unsupported("the CSV quote " + quote);
  }
  return quote.front();
}

void CheckCsvNullMarkers(bool null_marker, bool null_markers) {
  if (null_marker && null_markers) {
    throw ApiError::Invalid(
        "null_marker and null_markers cannot be used at the same time. Please use null_markers to "
        "specify the NULL values in your CSV file.");
  }
}

}  // namespace bigquery_emulator_duckdb
