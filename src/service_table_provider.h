#pragma once

#include <memory>

#include "src/external_query.h"
#include "src/external_table_provider.h"
#include "src/gcs.h"

namespace bigquery_emulator_duckdb {

// Connections outlive the provider. Deferred GCS reads retain their own client and metadata.
std::unique_ptr<ExternalTableProvider> NewServiceTableProvider(ExternalConnections& connections,
                                                               GcsClient gcs);

}  // namespace bigquery_emulator_duckdb
