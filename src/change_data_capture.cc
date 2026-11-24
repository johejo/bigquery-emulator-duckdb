#include "src/change_data_capture.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "absl/strings/match.h"
#include "src/duckdb_sql.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {

std::optional<ChangeType> ParseChangeType(std::string_view value) {
  if (absl::EqualsIgnoreCase(value, "INSERT")) {
    return ChangeType::kInsert;
  }
  if (absl::EqualsIgnoreCase(value, "UPSERT")) {
    return ChangeType::kUpsert;
  }
  if (absl::EqualsIgnoreCase(value, "DELETE")) {
    return ChangeType::kDelete;
  }
  return std::nullopt;
}

std::string ChangeSequence::Text() const {
  std::string text;
  for (const uint64_t section : sections) {
    text += std::format("{}{:016X}", text.empty() ? "" : "/", section);
  }
  return text;
}

std::variant<ChangeSequence, std::string> ParseChangeSequence(std::string_view text) {
  std::vector<std::string_view> sections;
  for (size_t start = 0;;) {
    const size_t slash = text.find('/', start);
    sections.push_back(text.substr(start, slash - start));
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  ChangeSequence sequence;
  if (std::ranges::any_of(sections, &std::string_view::empty)) {
    return "Empty segment is disallowed for the " + std::string(kChangeSequenceColumn) +
           ". Val: " + std::string(text);
  }
  for (const std::string_view section : sections) {
    uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(section.data(), section.data() + section.size(), value, 16);
    if (section.size() > 16 || error != std::errc() || end != section.data() + section.size()) {
      return "invalid hex segment for " + std::string(kChangeSequenceColumn) + ":" +
             std::string(section);
    }
    sequence.sections.push_back(value);
  }
  return sequence;
}

std::string ChangeSequenceTable(const std::string& project) {
  return QuoteIdentifier(project) + ".main.emulator_change_sequences";
}

std::string CreateChangeSequencesStatement(const std::string& project) {
  return std::format(
      "CREATE TABLE IF NOT EXISTS {} (dataset_id VARCHAR NOT NULL, table_id VARCHAR NOT NULL, "
      "creation_time BIGINT NOT NULL, key VARCHAR NOT NULL, sequence VARCHAR NOT NULL, "
      "deleted BOOLEAN NOT NULL, changed_micros BIGINT NOT NULL, "
      "PRIMARY KEY (dataset_id, table_id, creation_time, key))",
      ChangeSequenceTable(project));
}

std::string UpsertTablesTable(const std::string& project) {
  return QuoteIdentifier(project) + ".main.emulator_upsert_tables";
}

std::string CreateUpsertTablesStatement(const std::string& project) {
  return std::format(
      "CREATE TABLE IF NOT EXISTS {} (dataset_id VARCHAR NOT NULL, table_id VARCHAR NOT NULL, "
      "creation_time BIGINT NOT NULL, applied_millis BIGINT NOT NULL, "
      "PRIMARY KEY (dataset_id, table_id, creation_time))",
      UpsertTablesTable(project));
}

std::string ChangeSequencesOf(const TableReference& table, int64_t creation_time) {
  return std::format("dataset_id = {} AND table_id = {} AND creation_time = {}",
                     QuoteLiteral(table.dataset_id), QuoteLiteral(table.table_id), creation_time);
}

std::string UpsertApplyWatermarkQuery(const TableReference& table, int64_t creation_time) {
  return std::format("SELECT applied_millis FROM {} WHERE {}", UpsertTablesTable(table.project_id),
                     ChangeSequencesOf(table, creation_time));
}

std::string ChangeSequencesQuery(const TableReference& table, int64_t creation_time) {
  return std::format("SELECT 1 FROM {} WHERE {} LIMIT 1", ChangeSequenceTable(table.project_id),
                     ChangeSequencesOf(table, creation_time));
}

}  // namespace bigquery_emulator_duckdb
