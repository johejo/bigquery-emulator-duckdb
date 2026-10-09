#pragma once

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "googlesql/public/catalog.h"
#include "src/catalog.h"
#include "src/routine.h"

namespace googlesql {
class Function;
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// A BigQueryCatalog whose statements can also call the persistent SQL UDFs of its source, by a
// path that names their dataset. Each call resolves the routine's body anew, as BigQuery does:
// against the routine's own project, with no default dataset, so that the body names its tables
// and UDFs with their datasets, and without the temporary tables and UDFs of the job. The body of
// a routine with ANY TYPE arguments is resolved for the argument types of each call. Like the
// tables, the routines are kept for the lifetime of the catalog.
class RoutineCatalog : public BigQueryCatalog {
 public:
  // `source`, `type_factory` and `temporary` must outlive the catalog, as for BigQueryCatalog.
  RoutineCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                 std::string default_project, std::string default_dataset,
                 const TemporaryTables* temporary = nullptr);
  ~RoutineCatalog() override;

  absl::Status FindFunction(const absl::Span<const std::string>& path,
                            const googlesql::Function** function,
                            const FindOptions& options) override;

  // Resolves the body of `routine` as a call would, which checks a routine about to be created.
  absl::Status CheckRoutine(const Routine& routine);

 private:
  // What the catalog and the catalogs of the bodies it resolves share: the routines resolved so
  // far, those being resolved, which a body may not call, and what their functions borrow.
  struct State;

  RoutineCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                 std::string default_project, State* state);

  // The function `routine` defines, resolved as described above.
  absl::StatusOr<std::unique_ptr<googlesql::Function>> Resolve(const Routine& routine);

  std::unique_ptr<State> owned_state_;
  State* state_;
};

}  // namespace bigquery_emulator_duckdb
