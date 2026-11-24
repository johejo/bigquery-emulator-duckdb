#include "src/change_data_capture.h"

#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "src/duckdb_sql.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {

std::string ChangeSequence::Text() const {
  return std::format("{}:{:016X}/{:016X}/{:016X}/{:016X}", count, sections.at(0), sections.at(1),
                     sections.at(2), sections.at(3));
}

std::optional<ChangeSequence> ChangeSequence::FromText(std::string_view text) {
  if (text.size() < 2 || text.at(1) != ':' || text.at(0) < '1' || text.at(0) > '4') {
    return std::nullopt;
  }
  auto sequence = ParseChangeSequence(text.substr(2));
  if (!sequence || sequence->count != 4) {
    return std::nullopt;
  }
  sequence->count = text.at(0) - '0';
  return sequence;
}

std::optional<ChangeSequence> ParseChangeSequence(std::string_view text) {
  ChangeSequence sequence;
  while (true) {
    if (sequence.count == 4) {
      return std::nullopt;
    }
    const size_t slash = text.find('/');
    const std::string_view section = text.substr(0, slash);
    if (section.empty() || section.size() > 16) {
      return std::nullopt;
    }
    uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(section.data(), section.data() + section.size(), value, 16);
    if (error != std::errc() || end != section.data() + section.size()) {
      return std::nullopt;
    }
    sequence.sections.at(sequence.count++) = value;
    if (slash == std::string_view::npos) {
      return sequence;
    }
    text.remove_prefix(slash + 1);
  }
}

std::optional<std::strong_ordering> CompareChangeSequences(const ChangeSequence& a,
                                                           const ChangeSequence& b) {
  const auto order = a.sections <=> b.sections;
  if (order == std::strong_ordering::equal && a.count != b.count) {
    return std::nullopt;
  }
  return order;
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

std::string ChangeSequencesOf(const TableReference& table, int64_t creation_time) {
  return std::format("dataset_id = {} AND table_id = {} AND creation_time = {}",
                     QuoteLiteral(table.dataset_id), QuoteLiteral(table.table_id), creation_time);
}

}  // namespace bigquery_emulator_duckdb
