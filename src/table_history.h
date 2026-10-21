#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {

class Backend;

// The rows a table had before each write, which FOR SYSTEM_TIME AS OF reads. A project keeps them
// in its main schema: `main.emulator_table_history` lists the versions of each table, named by its
// dataset, table and creationTime, so that a table dropped or replaced since does not share them,
// and each version's rows are in a table of that table's own, beside its version number. A version
// of a table lasts until the write that replaced it committed, and its rows follow it when it is
// renamed. The emulator does not keep the rows a table had before its schema changed, which the
// history records as a version it did not keep, nor those of a table it replaced.

// How long BigQuery lets FOR SYSTEM_TIME AS OF read a table back, its default time travel window.
inline constexpr int64_t kTimeTravelWindowMillis = int64_t{7} * 24 * 60 * 60 * 1000;

// The statements that create the history of `project`, recording that it keeps no rows from
// before it was created, such as those of tables an earlier version of the emulator kept.
std::vector<std::string> CreateHistoryStatements(const std::string& project);

// The statements that keep the rows of `table`, created at `creation_time`, before a write to it in
// the same transaction, and drop the versions BigQuery no longer reads. A transaction keeps the
// rows once, before its first write to the table.
std::vector<std::string> KeepVersionStatements(const TableReference& table, int64_t creation_time);

// The statement that dates the versions the transaction kept in `project` to now, its commit.
std::string CommitVersionsStatement(const std::string& project);

// The statements that record that the rows of `table`, created at `creation_time`, are no longer
// kept from now back, run where its schema changes.
std::vector<std::string> DiscardHistoryStatements(const TableReference& table,
                                                  int64_t creation_time);

// The statements that move the history of `table`, created at `creation_time`, to the name
// `table_id` that it is renamed to.
std::vector<std::string> RenameHistoryStatements(const TableReference& table, int64_t creation_time,
                                                 const std::string& table_id);

// Drops the history of the tables of `project` that were dropped or replaced since. Another sweep
// running meanwhile can make it skip some, which a later one drops.
void SweepHistory(Backend& backend, const std::string& project);

// The query that reads the rows `table`, created at `creation_time`, had at `time`, in microseconds
// since the Unix epoch. InvalidArgument with BigQuery's message for a time it cannot read, and
// Unimplemented for one whose rows the emulator did not keep.
absl::StatusOr<std::string> TimeTravelQuery(Backend& backend, const TableReference& table,
                                            int64_t creation_time, int64_t time);

}  // namespace bigquery_emulator_duckdb
