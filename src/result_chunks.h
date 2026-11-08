#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace duckdb {
// Keep DuckDB's C++ result and vector headers out of the public result interface.
class QueryResult;
class DataChunk;
struct LogicalType;
}  // namespace duckdb

namespace bigquery_emulator_duckdb {

// Every chunk of a DuckDB query result, which query results and Storage Read snapshots keep
// instead of encoded rows. Chunks are only read once fetched, so threads may share them.
class ResultChunks {
 public:
  // Fetches every chunk of `result`. `owner`, which holds the database whose buffers the chunks
  // may pin, outlives them.
  ResultChunks(duckdb::QueryResult& result, std::shared_ptr<const void> owner);
  ~ResultChunks();
  ResultChunks(const ResultChunks&) = delete;
  ResultChunks& operator=(const ResultChunks&) = delete;
  ResultChunks(ResultChunks&&) = delete;
  ResultChunks& operator=(ResultChunks&&) = delete;

  [[nodiscard]] size_t row_count() const { return ends_.empty() ? 0 : ends_.back(); }
  [[nodiscard]] size_t column_count() const;
  [[nodiscard]] const duckdb::LogicalType& type(size_t column) const;

  // Calls visit(chunk, begin, take, copied) for each run of rows [offset, offset + count) in
  // one chunk, where `copied` rows precede it.
  template <typename F>
  void Runs(size_t offset, size_t count, const F& visit) const {
    size_t copied = 0;
    size_t index = std::ranges::upper_bound(ends_, offset) - ends_.begin();
    while (copied < count) {
      const size_t begin = index == 0 ? 0 : ends_.at(index - 1);
      const size_t take = std::min(count - copied, ends_.at(index) - offset);
      visit(ChunkAt(index), offset - begin, take, copied);
      copied += take;
      offset += take;
      ++index;
    }
  }

 private:
  // Declared first, so that it is destroyed after the chunks.
  std::shared_ptr<const void> owner_;
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<size_t> ends_;
  [[nodiscard]] const duckdb::DataChunk& ChunkAt(size_t index) const;
};

}  // namespace bigquery_emulator_duckdb
