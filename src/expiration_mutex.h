#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
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
// instead holds a RowWrites, which lets such statements run concurrently while drops wait for
// them, since a drop that commits during their transaction conflicts with it.
class ExpirationMutex {
 public:
  void lock() { catalog_.lock(); }
  void unlock() { catalog_.unlock(); }
  bool try_lock() { return catalog_.try_lock(); }

  // Keeps drops out until it is destroyed. A statement acquires it after expiring tables and
  // before opening its snapshot, and must not lock this mutex while it holds it.
  class RowWrites {
   public:
    explicit RowWrites(ExpirationMutex& mutex);
    ~RowWrites();
    RowWrites(const RowWrites&) = delete;
    RowWrites& operator=(const RowWrites&) = delete;
    RowWrites(RowWrites&& other) noexcept;
    RowWrites& operator=(RowWrites&&) = delete;

   private:
    std::shared_lock<std::shared_mutex> lock_;
  };

  // Called while holding this mutex: waits for row writes to finish and keeps new ones out
  // until the result is destroyed. Returns nullopt, and the drop must be skipped, when the calling
  // thread holds a RowWrites itself; the next sweep drops the relation.
  [[nodiscard]] std::optional<std::unique_lock<std::shared_mutex>> LockDrop();

 private:
  std::recursive_mutex catalog_;
  std::shared_mutex rows_;
};

// The coordination a statement holds while it runs.
struct StatementLock {
  std::unique_lock<ExpirationMutex> catalog;
  std::optional<ExpirationMutex::RowWrites> rows;
};

}  // namespace bigquery_emulator_duckdb
