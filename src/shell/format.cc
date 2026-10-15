#include "src/shell/format.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb::shell {

namespace {

using nlohmann::json;
// Rows keep the schema's field order.
using nlohmann::ordered_json;

// The value of `cell`, a row's {"v": ...}, with RECORD values as objects keyed by field name and
// REPEATED values as arrays. Scalars stay strings, as the API encodes them.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a field and its value.
ordered_json CellValue(const json& field, const json& value, bool element = false) {
  if (value.is_null()) {
    return nullptr;
  }
  if (!element && field.value("mode", "") == "REPEATED") {
    ordered_json values = ordered_json::array();
    for (const json& item : value) {
      values.push_back(CellValue(field, item.at("v"), true));
    }
    return values;
  }
  const std::string type = field.value("type", "");
  if (type == "RECORD" || type == "STRUCT") {
    ordered_json record = ordered_json::object();
    const json& fields = field.at("fields");
    const json& cells = value.at("f");
    for (size_t i = 0; i < fields.size(); ++i) {
      record[fields[i].at("name").get<std::string>()] = CellValue(fields[i], cells[i].at("v"));
    }
    return record;
  }
  return value;
}

std::vector<ordered_json> RowObjects(const json& response) {
  std::vector<ordered_json> rows;
  const json& fields = response.at("schema").at("fields");
  for (const json& row : response.value("rows", json::array())) {
    ordered_json object = ordered_json::object();
    const json& cells = row.at("f");
    for (size_t i = 0; i < fields.size(); ++i) {
      object[fields[i].at("name").get<std::string>()] = CellValue(fields[i], cells[i].at("v"));
    }
    rows.push_back(std::move(object));
  }
  return rows;
}

// A scalar as its text and anything else as JSON; `null_text` stands for NULL.
std::string CellText(const ordered_json& value, std::string_view null_text) {
  if (value.is_null()) {
    return std::string(null_text);
  }
  return value.is_string() ? value.get<std::string>() : value.dump();
}

// The width of UTF-8 `text` in code points.
size_t Width(std::string_view text) {
  return static_cast<size_t>(std::ranges::count_if(
      text, [](char byte) { return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U; }));
}

void WritePretty(const std::vector<std::string>& names, const std::vector<ordered_json>& rows,
                 std::ostream& out) {
  std::vector<std::vector<std::string>> cells;
  std::vector<size_t> widths;
  std::ranges::transform(names, std::back_inserter(widths),
                         [](const std::string& name) { return Width(name); });
  for (const ordered_json& row : rows) {
    std::vector<std::string>& texts = cells.emplace_back();
    for (size_t i = 0; i < names.size(); ++i) {
      texts.push_back(CellText(row.at(names.at(i)), "NULL"));
      widths.at(i) = std::max(widths.at(i), Width(texts.back()));
    }
  }
  std::string rule = "+";
  for (const size_t width : widths) {
    rule += std::string(width + 2, '-') + "+";
  }
  const auto write_row = [&](const std::vector<std::string>& texts) {
    out << '|';
    for (size_t i = 0; i < texts.size(); ++i) {
      out << ' ' << texts.at(i) << std::string(widths.at(i) - Width(texts.at(i)) + 1, ' ') << '|';
    }
    out << '\n';
  };
  out << rule << '\n';
  write_row(names);
  out << rule << '\n';
  for (const auto& texts : cells) {
    write_row(texts);
  }
  if (!cells.empty()) {
    out << rule << '\n';
  }
}

// RFC 4180 quoting; NULL is an empty field.
void WriteCsvField(const ordered_json& value, std::ostream& out) {
  const std::string text = CellText(value, "");
  if (text.find_first_of(",\"\r\n") == std::string::npos) {
    out << text;
    return;
  }
  out << '"';
  for (const char c : text) {
    if (c == '"') {
      out << '"';
    }
    out << c;
  }
  out << '"';
}

void WriteCsv(const std::vector<std::string>& names, const std::vector<ordered_json>& rows,
              std::ostream& out) {
  for (size_t i = 0; i < names.size(); ++i) {
    out << (i == 0 ? "" : ",");
    WriteCsvField(names.at(i), out);
  }
  out << "\r\n";
  for (const ordered_json& row : rows) {
    for (size_t i = 0; i < names.size(); ++i) {
      out << (i == 0 ? "" : ",");
      WriteCsvField(row.at(names.at(i)), out);
    }
    out << "\r\n";
  }
}

}  // namespace

std::optional<OutputFormat> ParseOutputFormat(std::string_view name) {
  if (name == "pretty") {
    return OutputFormat::kPretty;
  }
  if (name == "json") {
    return OutputFormat::kJson;
  }
  if (name == "jsonl") {
    return OutputFormat::kJsonl;
  }
  if (name == "csv") {
    return OutputFormat::kCsv;
  }
  return std::nullopt;
}

// Writes the rows of a jobs.query response, whose values keep the API's encoding.
void WriteRows(const json& response, OutputFormat format, std::ostream& out) {
  std::vector<std::string> names;
  std::ranges::transform(response.at("schema").at("fields"), std::back_inserter(names),
                         [](const json& field) { return field.at("name").get<std::string>(); });
  const std::vector<ordered_json> rows = RowObjects(response);
  switch (format) {
    case OutputFormat::kPretty:
      WritePretty(names, rows, out);
      break;
    case OutputFormat::kJson:
      out << ordered_json(rows).dump() << '\n';
      break;
    case OutputFormat::kJsonl:
      for (const ordered_json& row : rows) {
        out << row.dump() << '\n';
      }
      break;
    case OutputFormat::kCsv:
      WriteCsv(names, rows, out);
      break;
  }
}

}  // namespace bigquery_emulator_duckdb::shell
