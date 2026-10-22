#pragma once

#include <cstddef>
#include <iterator>
#include <memory>

#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {

class ResultChunks;

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
  // A NULL array is an empty one, as BigQuery returns it, unless `null_arrays` is set.
  ResultRows(std::shared_ptr<const ResultChunks> chunks, bool null_arrays);

  [[nodiscard]] size_t size() const;
  [[nodiscard]] bool empty() const { return size() == 0; }
  // Encodes row `row`; throws std::out_of_range past the last one.
  [[nodiscard]] nlohmann::json at(size_t row) const;
  [[nodiscard]] Iterator begin() const { return {this, 0}; }
  [[nodiscard]] Iterator end() const { return {this, size()}; }

 private:
  std::shared_ptr<const ResultChunks> chunks_;
  bool null_arrays_ = false;
};

}  // namespace bigquery_emulator_duckdb
