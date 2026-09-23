#include "src/duckdb_sql.h"

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bigquery_emulator_duckdb {
namespace {

// Type names that differ between GoogleSQL and DuckDB.
const std::unordered_map<std::string, std::string>& TypeNames() {
  static const auto* const kNames = new std::unordered_map<std::string, std::string>{
      {"INT64", "BIGINT"},
      {"FLOAT64", "DOUBLE"},
      {"BOOL", "BOOLEAN"},
      {"STRING", "VARCHAR"},
      {"BYTES", "BLOB"},
      {"NUMERIC", "DECIMAL(38, 9)"},
      {"BIGNUMERIC", "DECIMAL(38, 19)"},
      // BigQuery TIMESTAMP is an absolute instant; DATETIME is a civil time.
      {"TIMESTAMP", "TIMESTAMPTZ"},
      {"DATETIME", "TIMESTAMP"},
  };
  return *kNames;
}

}  // namespace

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

std::string DuckDbTypeName(std::string_view googlesql_name, bool has_type_parameters) {
  const auto it = TypeNames().find(ToUpperAscii(googlesql_name));
  if (it == TypeNames().end()) {
    return std::string(googlesql_name);
  }
  return has_type_parameters ? it->second.substr(0, it->second.find('(')) : it->second;
}

}  // namespace bigquery_emulator_duckdb
