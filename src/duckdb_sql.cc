#include "src/duckdb_sql.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace bigquery_emulator_duckdb {

std::string DuckPGQName(const std::vector<std::string>& path) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result = "_bq_graph";
  for (size_t i = 0; i < path.size(); ++i) {
    result += '_';
    // Graph names ignore case, but their resolved dataset identities do not.
    for (const unsigned char c : i + 1 == path.size() ? ToUpperAscii(path.at(i)) : path.at(i)) {
      result += digits[c / 16];
      result += digits[c % 16];
    }
  }
  return QuoteIdentifier(result);
}

std::string ToUpperAscii(std::string_view text) {
  std::string result(text);
  std::ranges::transform(result, result.begin(), [](char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  });
  return result;
}

std::string ToLowerAscii(std::string_view text) {
  std::string result(text);
  std::ranges::transform(result, result.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return result;
}

std::string QuoteLiteral(std::string_view value) {
  // The SQL parser treats NUL as the end of its input. Decode UTF-8 bytes instead
  // so embedded NULs never appear in the statement text.
  if (value.find('\0') != std::string_view::npos) {
    return "decode(from_hex('" + ToHex(value) + "'))";
  }
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

std::string DatasetCatalogName(std::string_view name) {
  // Lowercase ASCII identifiers already have case-sensitive identities in DuckDB.
  // Escape the encoding's prefix too, so a logical name cannot alias an encoded one.
  if (!name.starts_with("_bq_") &&
      !std::ranges::any_of(name, [](unsigned char c) { return c >= 'A' && c <= 'Z'; })) {
    return std::string(name);
  }
  return "_bq_" + ToHex(name);
}

std::string TableCatalogName(std::string_view dataset, std::string_view name) {
  return dataset == "main" || dataset == "information_schema" || dataset == "pg_catalog"
             ? std::string(name)
             : DatasetCatalogName(name);
}

std::string LogicalCatalog(std::string_view relation) {
  const auto decode = [relation](std::string_view column) {
    const std::string name(column);
    const std::string schema =
        relation == "information_schema.tables" ? "table_schema" : "schema_name";
    // Native tables, including the emulator's main schema, do not use encoded names.
    const std::string scope =
        column == "table_name" || column == "view_name"
            ? " AND " + schema + " NOT IN ('main', 'information_schema', 'pg_catalog')"
            : "";
    return "CASE WHEN regexp_full_match(" + name + ", '_bq_([0-9a-f]{2})*')" + scope +
           " THEN decode(from_hex(substr(" + name + ", 5))) ELSE " + name + " END AS " + name;
  };
  std::vector<std::string> columns;
  if (relation == "information_schema.schemata" || relation == "duckdb_functions()") {
    columns = {"schema_name"};
  } else if (relation == "information_schema.tables") {
    columns = {"table_schema", "table_name"};
  } else {
    columns = {"schema_name", relation == "duckdb_views()" ? "view_name" : "table_name"};
  }
  std::vector<std::string> expressions;
  std::ranges::transform(columns, std::back_inserter(expressions), decode);
  return "(SELECT * REPLACE (" + Join(expressions, ", ") + ") FROM " + std::string(relation) + ")";
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
             QuoteIdentifier(
                 DatasetCatalogName(path.substr(dataset_dot + 1, table_dot - dataset_dot - 1))) +
             "." +
             QuoteIdentifier(
                 TableCatalogName(path.substr(dataset_dot + 1, table_dot - dataset_dot - 1),
                                  path.substr(table_dot + 1)));
    }
  }
  const size_t dataset_dot = path.find('.');
  const size_t table_dot = dataset_dot == std::string_view::npos ? std::string_view::npos
                                                                 : path.find('.', dataset_dot + 1);
  if (table_dot == std::string_view::npos) {
    return QuoteIdentifier(path);
  }
  const auto dataset = path.substr(dataset_dot + 1, table_dot - dataset_dot - 1);
  return QuoteIdentifier(path.substr(0, dataset_dot)) + "." +
         QuoteIdentifier(DatasetCatalogName(dataset)) + "." +
         QuoteIdentifier(TableCatalogName(dataset, path.substr(table_dot + 1)));
}

std::string ToHex(std::string_view value) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    result += kDigits.at(byte >> 4U);
    result += kDigits.at(byte & 0x0FU);
  }
  return result;
}

std::string Join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string result;
  for (const auto& part : parts) {
    if (!result.empty()) {
      result += separator;
    }
    result += part;
  }
  return result;
}

}  // namespace bigquery_emulator_duckdb
