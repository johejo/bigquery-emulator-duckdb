#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "src/references.h"

namespace bigquery_emulator_duckdb {

// The pseudocolumns of a Storage Write change data capture row,
// third_party/bigquery_docs/change-data-capture.md.
inline constexpr std::string_view kChangeTypeColumn = "_CHANGE_TYPE";
inline constexpr std::string_view kChangeSequenceColumn = "_CHANGE_SEQUENCE_NUMBER";

// A `_CHANGE_SEQUENCE_NUMBER`: one to four sections of at most 16 hexadecimal digits, separated by
// slashes, which BigQuery compares as unsigned integers, section by section.
struct ChangeSequence {
  std::array<uint64_t, 4> sections = {};
  int count = 0;

  // The text the change sequence table stores: the count, then all four sections.
  [[nodiscard]] std::string Text() const;
  // Reads Text() back; nullopt for any other text.
  static std::optional<ChangeSequence> FromText(std::string_view text);
};

// nullopt for text not in the fixed format, including the empty string.
std::optional<ChangeSequence> ParseChangeSequence(std::string_view text);

// How `a` orders against `b`, comparing missing sections as zero. nullopt for sequences that are
// equal so but have different numbers of sections, such as 'A' and 'A/0', whose order BigQuery
// does not document.
std::optional<std::strong_ordering> CompareChangeSequences(const ChangeSequence& a,
                                                           const ChangeSequence& b);

// The table that records, for each primary key that a change data capture row with a sequence
// number changed, the sequence number of the change that took effect, whether it deleted the key,
// and when, so that a later change with a lower sequence number does not take effect. Its rows
// name a table by its dataset, table and creation time, as its time travel history does, since
// DuckDB renumbers table OIDs when it opens a database, and a key by the canonical JSON array of
// its values. A table with a primary key cannot be renamed, so they need not follow renames.
std::string ChangeSequenceTable(const std::string& project);
std::string CreateChangeSequencesStatement(const std::string& project);
// The condition that selects the rows of `table`, created at `creation_time`.
std::string ChangeSequencesOf(const TableReference& table, int64_t creation_time);

// How long BigQuery keeps a deleted key's sequence number to order the changes that arrive after
// it out of order.
inline constexpr int64_t kDeleteRetentionMicros = int64_t{2} * 24 * 60 * 60 * 1000 * 1000;

}  // namespace bigquery_emulator_duckdb
