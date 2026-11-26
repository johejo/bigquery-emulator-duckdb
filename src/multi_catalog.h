#pragma once

#include <memory>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/multi_catalog.h"

namespace bigquery_emulator_duckdb {

// As googlesql::MultiCatalog::Create, for a catalog that also finds connections, in the first of
// `catalogs` that has one; GoogleSQL's MultiCatalog looks for connections only by name.
absl::Status CreateMultiCatalog(absl::string_view name,
                                const std::vector<googlesql::Catalog*>& catalogs,
                                std::unique_ptr<googlesql::MultiCatalog>* multi_catalog);

}  // namespace bigquery_emulator_duckdb
