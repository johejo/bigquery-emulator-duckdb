#include "src/duckdb_sql.h"

#include <cctype>
#include <string>
#include <string_view>

namespace bigquery_emulator_duckdb {
std::string ToUpperAscii(std::string_view text) {
  std::string result(text);
  for (char& c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

std::string ToLowerAscii(std::string_view text) {
  std::string result(text);
  for (char& c : result) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

std::string QuoteLiteral(std::string_view value) {
  std::string result = "'";
  for (const char c : value) {
    result += c;
    if (c == '\'') {
      result += c;
    }
  }
  result += '\'';
  return result;
}

std::string QuoteIdentifier(std::string_view name) {
  std::string result = "\"";
  for (const char c : name) {
    result += c;
    if (c == '"') {
      result += c;
    }
  }
  result += '"';
  return result;
}

std::string QuoteIdentifierPath(std::string_view path) {
  // A domain-scoped project ID can contain dots. Only the final two dots separate
  // project, dataset, and table in a resolved table's full name.
  if (path.find(':') != std::string_view::npos) {
    const size_t table_dot = path.rfind('.');
    const size_t dataset_dot = table_dot == std::string_view::npos ? std::string_view::npos
                                                                   : path.rfind('.', table_dot - 1);
    if (dataset_dot != std::string_view::npos) {
      return QuoteIdentifier(path.substr(0, dataset_dot)) + "." +
             QuoteIdentifier(path.substr(dataset_dot + 1, table_dot - dataset_dot - 1)) + "." +
             QuoteIdentifier(path.substr(table_dot + 1));
    }
  }
  std::string result = "\"";
  for (const char c : path) {
    if (c == '.') {
      result += "\".\"";
    } else {
      result += c;
      if (c == '"') {
        result += c;
      }
    }
  }
  result += '"';
  return result;
}

std::string ToHex(std::string_view value) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    result += kDigits[byte >> 4U];
    result += kDigits[byte & 0x0FU];
  }
  return result;
}

}  // namespace bigquery_emulator_duckdb
