#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "src/references.h"

namespace bigquery_emulator_duckdb {

// The pseudocolumns of a Storage Write change data capture row,
// third_party/bigquery_docs/change-data-capture.md.
inline constexpr std::string_view kChangeTypeColumn = "_CHANGE_TYPE";
inline constexpr std::string_view kChangeSequenceColumn = "_CHANGE_SEQUENCE_NUMBER";

// What a `_CHANGE_TYPE` asks for. A connection writes either inserts, as every row without a
// `_CHANGE_TYPE` does, or upserts and deletes, which its first row decides.
enum class ChangeType : uint8_t { kInsert, kUpsert, kDelete };

// The change type `value` names, ignoring case, or nullopt for a value BigQuery rejects.
std::optional<ChangeType> ParseChangeType(std::string_view value);

// A `_CHANGE_SEQUENCE_NUMBER`: sections of at most 16 hexadecimal digits, separated by slashes,
// which BigQuery compares as unsigned integers, section by section. A sequence number that
// another one begins orders before it, so 'A' is lower than 'A/0'.
struct ChangeSequence {
  std::vector<uint64_t> sections;

  auto operator<=>(const ChangeSequence&) const = default;

  // The text the change sequence table stores, with 16 digits in each section.
  [[nodiscard]] std::string Text() const;
};

// The sequence number `text` spells, or the message of the row error BigQuery gives for it. Throws
// ApiError::Unsupported for a section that is not hexadecimal in text of several sections, for
// which BigQuery's message is not known.
std::variant<ChangeSequence, std::string> ParseChangeSequence(std::string_view text);

// The table that records, for each primary key that a change data capture row with a sequence
// number changed, the sequence number of the change that took effect, whether it deleted the key,
// and when, so that a later change with a lower sequence number does not take effect. Its rows
// name a table by its dataset, table and creation time, as its time travel history does, since
// DuckDB renumbers table OIDs when it opens a database, and a key by the canonical JSON array of
// its values. A table with a primary key cannot be renamed, so they need not follow renames.
std::string ChangeSequenceTable(const std::string& project);
std::string CreateChangeSequencesStatement(const std::string& project);
// The table that records, by the same names, the tables that UPSERT and DELETE rows changed and
// when they last did, which is the table's `upsert_stream_apply_watermark`.
std::string UpsertTablesTable(const std::string& project);
std::string CreateUpsertTablesStatement(const std::string& project);
// The condition that selects the rows of `table`, created at `creation_time`, in either table.
std::string ChangeSequencesOf(const TableReference& table, int64_t creation_time);
// The query of the watermark, in milliseconds since the Unix epoch, of `table`, created at
// `creation_time`, which has no row when no UPSERT or DELETE row changed the table.
std::string UpsertApplyWatermarkQuery(const TableReference& table, int64_t creation_time);

// How long BigQuery keeps a deleted key's sequence number to order the changes that arrive after
// it out of order.
inline constexpr int64_t kDeleteRetentionMicros = int64_t{2} * 24 * 60 * 60 * 1000 * 1000;

}  // namespace bigquery_emulator_duckdb
