#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include "absl/synchronization/mutex.h"

namespace bigquery_emulator_duckdb {

// What a statement may change, which decides the expiration coordination it holds.
enum class StatementWrites : std::uint8_t {
  kNothing,  // A query, which reads only its own snapshot.
  kRows,     // DML, which changes rows but neither creates nor drops relations.
  kCatalog,  // Anything else, such as DDL.
};

// Coordinates lazy expiration with writes. Catalog mutations and expiration drops hold the
// catalog lock. A statement that writes only rows instead holds LockRowWrites(), which lets
// such statements run concurrently while drops wait for them, since a drop that commits during
// their transaction conflicts with it.
class ExpirationMutex {
 public:
  // Movable ownership lets statement coordination outlive the call that acquires it. Pass a
  // held lock to nested catalog reads and sweeps instead of acquiring the catalog lock again.
  using CatalogLock = std::unique_lock<absl::Mutex>;

  [[nodiscard]] CatalogLock LockCatalog() { return CatalogLock(catalog_); }
  void AssertCatalogHeld() const { catalog_.AssertHeld(); }

  // Keeps drops out until it is released. A statement acquires it after expiring tables and
  // before opening its snapshot. While a thread holds it, that thread must neither acquire the
  // catalog lock nor expire tables, since the drop would wait for the thread itself.
  [[nodiscard]] std::shared_lock<absl::Mutex> LockRowWrites() { return std::shared_lock(rows_); }

  // Called while holding the catalog lock: waits for row writes to finish and keeps new ones
  // out until it is released.
  [[nodiscard]] std::unique_lock<absl::Mutex> LockDrop() { return std::unique_lock(rows_); }

 private:
  absl::Mutex catalog_;
  absl::Mutex rows_;
};

// The coordination a statement holds while it runs. Standard guards supply movable ownership
// over Abseil mutexes; a query leaves both empty.
struct StatementLock {
  ExpirationMutex::CatalogLock catalog;
  std::shared_lock<absl::Mutex> rows;
};

}  // namespace bigquery_emulator_duckdb
