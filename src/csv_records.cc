#include "src/csv_records.h"

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/api_error.h"

namespace bigquery_emulator_duckdb {

std::vector<CsvRecord> ParseCsvRecords(std::string_view contents, const CsvDialect& dialect,
                                       size_t limit, bool lenient) {
  std::vector<CsvRecord> records;
  size_t line = 1;
  size_t i = 0;
  const auto line_break = [&] { return contents.at(i) == '\n' || contents.at(i) == '\r'; };
  const auto line_end = [&] { return i >= contents.size() || line_break(); };
  while (i < contents.size() && records.size() < limit) {
    CsvRecord record;
    const size_t record_line = line;
    while (true) {
      CsvCell cell;
      if (dialect.quote && i < contents.size() && contents.at(i) == *dialect.quote) {
        cell.quoted = true;
        for (++i;; ++i) {
          if (i >= contents.size() || (!dialect.allow_quoted_newlines && line_break())) {
            if (lenient) {
              break;
            }
            throw ApiError::Invalid(
                std::format("Error while reading data, error message: Missing close quote "
                            "character ({}).; line_number: {}",
                            *dialect.quote, record_line));
          }
          if (contents.at(i) == *dialect.quote) {
            if (i + 1 >= contents.size() || contents.at(i + 1) != *dialect.quote) {
              ++i;
              break;
            }
            ++i;
          } else if (contents.at(i) == '\n') {
            ++line;
          }
          cell.text += contents.at(i);
        }
      }
      for (; !line_end() && contents.at(i) != dialect.delimiter; ++i) {
        cell.text += contents.at(i);
      }
      record.push_back(cell.quoted || !cell.text.empty() ? std::optional(std::move(cell))
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
    if (record.size() > 1 || record.front().has_value()) {
      records.push_back(std::move(record));
    }
  }
  return records;
}

}  // namespace bigquery_emulator_duckdb
