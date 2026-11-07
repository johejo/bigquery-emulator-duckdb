#include "src/expiration_mutex.h"

#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

namespace bigquery_emulator_duckdb {

namespace {

// How many RowWrites the calling thread holds; a drop it attempted would wait for itself.
thread_local int row_writes_held = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace

ExpirationMutex::RowWrites::RowWrites(ExpirationMutex& mutex) : lock_(mutex.rows_) {
  ++row_writes_held;
}

ExpirationMutex::RowWrites::~RowWrites() {
  if (lock_.owns_lock()) {
    --row_writes_held;
  }
}

ExpirationMutex::RowWrites::RowWrites(RowWrites&& other) noexcept : lock_(std::move(other.lock_)) {}

std::optional<std::unique_lock<std::shared_mutex>> ExpirationMutex::LockDrop() {
  if (row_writes_held > 0) {
    return std::nullopt;
  }
  return std::optional<std::unique_lock<std::shared_mutex>>(std::in_place, rows_);
}

}  // namespace bigquery_emulator_duckdb
