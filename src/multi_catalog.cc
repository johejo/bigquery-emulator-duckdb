#include "src/multi_catalog.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/multi_catalog.h"

namespace bigquery_emulator_duckdb {
namespace {

class ConnectionMultiCatalog : public googlesql::MultiCatalog {
 public:
  ConnectionMultiCatalog(absl::string_view name, const std::vector<googlesql::Catalog*>& catalogs)
      : MultiCatalog(name, catalogs) {}

  absl::Status FindConnection(const absl::Span<const std::string>& path,
                              const googlesql::Connection** connection,
                              const FindOptions& options) override {
    for (googlesql::Catalog* catalog : catalogs()) {
      const absl::Status status = catalog->FindConnection(path, connection, options);
      if (status.code() != absl::StatusCode::kNotFound) {
        return status;
      }
    }
    *connection = nullptr;
    return absl::NotFoundError("Connection not found: " + absl::StrJoin(path, "."));
  }
};

}  // namespace

absl::Status CreateMultiCatalog(absl::string_view name,
                                const std::vector<googlesql::Catalog*>& catalogs,
                                std::unique_ptr<googlesql::MultiCatalog>* multi_catalog) {
  if (std::ranges::any_of(catalogs,
                          [](const googlesql::Catalog* catalog) { return catalog == nullptr; })) {
    return absl::InvalidArgumentError("A MultiCatalog takes no null catalogs");
  }
  *multi_catalog = std::make_unique<ConnectionMultiCatalog>(name, catalogs);
  return absl::OkStatus();
}

}  // namespace bigquery_emulator_duckdb
