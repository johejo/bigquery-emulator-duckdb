#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>

namespace bigquery_emulator_duckdb {

// What a statement may change, which decides the expiration coordination it holds.
enum class StatementWrites : std::uint8_t {
  kNothing,  // A query, which reads only its own snapshot.
  kRows,     // DML, which changes rows but neither creates nor drops relations.
  kCatalog,  // Anything else, such as DDL.
};

// Coordinates lazy expiration with writes. Locking it, recursively, serializes catalog mutations
// with each other and with the drops of expired relations. A statement that writes only rows
// instead holds LockRowWrites(), which lets such statements run concurrently while drops wait for
// them, since a drop that commits during their transaction conflicts with it.
class ExpirationMutex {
 public:
  void lock() { catalog_.lock(); }
  void unlock() { catalog_.unlock(); }
  bool try_lock() { return catalog_.try_lock(); }

  // Keeps drops out until it is released. A statement acquires it after expiring tables and
  // before opening its snapshot. While a thread holds it, that thread must neither lock this
  // mutex nor expire tables, since the drop would wait for the thread itself.
  [[nodiscard]] std::shared_lock<std::shared_mutex> LockRowWrites() {
    return std::shared_lock(rows_);
  }

  // Called while holding this mutex: waits for row writes to finish and keeps new ones out until
  // it is released.
  [[nodiscard]] std::unique_lock<std::shared_mutex> LockDrop() { return std::unique_lock(rows_); }

 private:
  std::recursive_mutex catalog_;
  std::shared_mutex rows_;
};

// The coordination a statement holds while it runs.
struct StatementLock {
  std::unique_lock<ExpirationMutex> catalog;
  std::shared_lock<std::shared_mutex> rows;
};

}  // namespace bigquery_emulator_duckdb
