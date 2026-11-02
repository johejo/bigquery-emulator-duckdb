#pragma once

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "src/field_schema.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {

// Application-supplied external reads for the native catalog. Read definitions must own any
// state needed after this provider and its table source are destroyed; resolution must not stage
// files. Connection discovery may register shared database macros, outside a script transaction.
class ExternalTableProvider {
 public:
  virtual ~ExternalTableProvider() = default;
  // `table_name`, as project.dataset.table, names the table in read errors.
  virtual TableRead Read(GcsExternalData definition, std::vector<FieldSchema> schema,
                         std::string table_name) = 0;
  // The schema of the external table `table_id` created without one, read from its sources when
  // it is created, as BigQuery detects it. Throws ApiError.
  virtual std::vector<FieldSchema> DetectSchema(const GcsExternalData& definition,
                                                const std::string& table_id) = 0;
  virtual absl::StatusOr<std::vector<FieldSchema>> DescribeQuery(const std::string& key,
                                                                 const std::string& query,
                                                                 const std::string& options,
                                                                 TableSource& source) = 0;
};

}  // namespace bigquery_emulator_duckdb
