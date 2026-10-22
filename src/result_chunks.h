#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "duckdb.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb {

// Every chunk of a DuckDB query result, which query results and Storage Read snapshots keep
// instead of encoded rows. Chunks are only read once fetched, so threads may share them.
class ResultChunks {
 public:
  // Fetches every chunk of `result`. `owner`, which holds the database whose buffers the chunks
  // may pin, outlives them.
  ResultChunks(duckdb_result& result, std::shared_ptr<const void> owner)
      : owner_(std::move(owner)) {
    for (idx_t i = 0; i < duckdb_column_count(&result); ++i) {
      types_.emplace_back(duckdb_column_logical_type(&result, i));
    }
    size_t count = 0;
    while (true) {
      Chunk chunk(duckdb_fetch_chunk(result));
      if (chunk.get() == nullptr) {
        break;
      }
      count += duckdb_data_chunk_get_size(chunk.get());
      chunks_.push_back(std::move(chunk));
      ends_.push_back(count);
    }
  }
  ~ResultChunks() = default;
  ResultChunks(const ResultChunks&) = delete;
  ResultChunks& operator=(const ResultChunks&) = delete;
  ResultChunks(ResultChunks&&) = delete;
  ResultChunks& operator=(ResultChunks&&) = delete;

  [[nodiscard]] size_t row_count() const { return ends_.empty() ? 0 : ends_.back(); }
  [[nodiscard]] size_t column_count() const { return types_.size(); }
  [[nodiscard]] duckdb_logical_type type(size_t column) const { return types_.at(column).get(); }

  // Calls visit(chunk, begin, take, copied) for each run of rows [offset, offset + count) in
  // one chunk, where `copied` rows precede it.
  template <typename F>
  void Runs(size_t offset, size_t count, const F& visit) const {
    size_t copied = 0;
    size_t index = std::ranges::upper_bound(ends_, offset) - ends_.begin();
    while (copied < count) {
      const size_t begin = index == 0 ? 0 : ends_.at(index - 1);
      const size_t take = std::min(count - copied, ends_.at(index) - offset);
      visit(chunks_.at(index).get(), offset - begin, take, copied);
      copied += take;
      offset += take;
      ++index;
    }
  }

 private:
  // Declared first, so that it is destroyed after the chunks.
  std::shared_ptr<const void> owner_;
  std::vector<LogicalType> types_;
  std::vector<Chunk> chunks_;
  std::vector<size_t> ends_;
};

}  // namespace bigquery_emulator_duckdb
