#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>

#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {

class ResultChunks;

// How a row encodes a TIMESTAMP: as epoch microseconds, which the emulator reads and clients get
// with formatOptions.useInt64Timestamp, or as BigQuery's default seconds since the epoch.
enum class TimestampEncoding : uint8_t { kMicros, kSeconds };

// The rows of a query result, kept as the DuckDB chunks of its result. Each row is encoded in
// BigQuery's wire format ({"f": [{"v": ...}, ...]}) when it is read, and copies share the chunks.
class ResultRows {
 public:
  class Iterator {
   public:
    using iterator_concept = std::input_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type = nlohmann::json;
    using difference_type = std::ptrdiff_t;
    using reference = nlohmann::json;
    using pointer = void;

    Iterator() = default;
    Iterator(const ResultRows* rows, size_t row) : rows_(rows), row_(row) {}
    nlohmann::json operator*() const { return rows_->at(row_); }
    Iterator& operator++() {
      ++row_;
      return *this;
    }
    Iterator operator++(int) { return {rows_, row_++}; }
    bool operator==(const Iterator& other) const { return row_ == other.row_; }

   private:
    const ResultRows* rows_ = nullptr;
    size_t row_ = 0;
  };

  ResultRows() = default;
  // A NULL array is an empty one, as BigQuery returns it, unless `null_arrays` is set. Values
  // that can fail to encode, such as an INTERVAL or BIGNUMERIC out of range, are encoded now and
  // throw BackendError, so that the statement fails rather than a later read of its rows.
  ResultRows(std::shared_ptr<const ResultChunks> chunks, bool null_arrays);

  [[nodiscard]] size_t size() const;
  [[nodiscard]] bool empty() const { return size() == 0; }
  // Read-only access for value adapters without a REST round-trip. The chunks are borrowed
  // from this result, or nullptr for a default-constructed result.
  [[nodiscard]] const ResultChunks* chunks() const { return chunks_.get(); }
  [[nodiscard]] bool null_arrays() const { return null_arrays_; }
  // Encodes row `row`; throws std::out_of_range past the last one.
  [[nodiscard]] nlohmann::json at(size_t row,
                                  TimestampEncoding timestamps = TimestampEncoding::kMicros) const;
  // The value of `column` in row `row`, read from its vector without encoding the row: String
  // and OptionalString read a VARCHAR and Int64 a BIGINT. OptionalString returns nullopt for
  // NULL, where the others throw BackendError, as they do for a column of another type. Each
  // throws std::out_of_range past the last row or column.
  [[nodiscard]] std::string String(size_t row, size_t column) const;
  [[nodiscard]] std::optional<std::string> OptionalString(size_t row, size_t column) const;
  [[nodiscard]] int64_t Int64(size_t row, size_t column) const;
  [[nodiscard]] Iterator begin() const { return {this, 0}; }
  [[nodiscard]] Iterator end() const { return {this, size()}; }

 private:
  std::shared_ptr<const ResultChunks> chunks_;
  bool null_arrays_ = false;
};

}  // namespace bigquery_emulator_duckdb
