#include "src/service_table_provider.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/duckdb_sql.h"
#include "src/external_query.h"
#include "src/external_table.h"
#include "src/external_table_provider.h"
#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/location.h"
#include "src/mysql_query.h"
#include "src/spanner_query.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {
namespace {

// PROJECT.LOCATION.CONNECTION, where only the project may have dots.
std::optional<std::string> ConnectionLocation(const std::string& key) {
  const size_t connection = key.rfind('.');
  const size_t location = connection == std::string::npos || connection == 0
                              ? std::string::npos
                              : key.rfind('.', connection - 1);
  if (location == std::string::npos) {
    return std::nullopt;
  }
  return CanonicalLocation(key.substr(location + 1, connection - location - 1));
}

class ServiceTableProvider : public ExternalTableProvider {
 public:
  ServiceTableProvider(ExternalConnections& connections, GcsClient gcs)
      : connections_(connections), gcs_(std::move(gcs)) {}

  TableRead Read(GcsExternalData definition, std::vector<FieldSchema> schema,
                 std::string table_name) override {
    return GcsExternalRead(std::move(definition), std::move(schema), std::move(table_name), gcs_);
  }

  std::vector<FieldSchema> DetectSchema(const GcsExternalData& definition,
                                        const std::string& table_id) override {
    return DetectGcsExternalSchema(definition, table_id, gcs_);
  }

  // NOLINTBEGIN(bugprone-easily-swappable-parameters): a connection and its query options.
  absl::StatusOr<std::vector<FieldSchema>> DescribeQuery(const std::string& key,
                                                         const std::string& query,
                                                         const std::string& options,
                                                         TableSource& source) override {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto connection = connections_.conninfo.find(key);
    if (connection == connections_.conninfo.end()) {
      return absl::NotFoundError("Not found: Connection " + key);
    }
    if (connection->second == kCloudResourceConnection) {
      return absl::InvalidArgumentError("Connection " + key +
                                        " is not a Cloud SQL or Spanner connection");
    }
    if (const std::optional<std::string> location = ConnectionLocation(key); location.has_value()) {
      source.ReferenceLocation("Connection " + key, *location);
    }
    const bool spanner = connection->second.starts_with("spanner://");
    const bool mysql = connection->second.starts_with("mysql:");
    absl::StatusOr<std::vector<FieldSchema>> columns =
        [&]() -> absl::StatusOr<std::vector<FieldSchema>> {
      if (spanner) {
        return DescribeSpannerQuery(connection->second.substr(10), query, options);
      }
      const auto decimal_type = DecimalColumnType(options);
      if (!decimal_type.ok()) {
        return decimal_type.status();
      }
      if (mysql) {
        return DescribeMySqlQuery(connection->second.substr(6), query, *decimal_type);
      }
      return DescribePostgresQuery(connection->second, query, *decimal_type);
    }();
    if (!columns.ok()) {
      return columns;
    }
    // Register once, on first use rather than startup when the database may not be up yet.
    // The shared backend registers outside any session's transaction.
    const std::scoped_lock lock(connections_.mutex);
    if (!connections_.attached.contains(key)) {
      try {
        const std::string name = QuoteIdentifier(ExternalDatabaseName(key));
        std::string read;
        if (spanner) {
          read = "bq_spanner_query(" + QuoteLiteral(connection->second.substr(10)) + ", sql)";
        } else if (mysql) {
          connections_.backend.Execute("ATTACH " + QuoteLiteral(connection->second.substr(6)) +
                                       " AS " + name + " (TYPE mysql, READ_ONLY)");
          read = "mysql_query(" + QuoteLiteral(ExternalDatabaseName(key)) + ", sql)";
        } else {
          connections_.backend.Execute("ATTACH " + QuoteLiteral(connection->second) + " AS " +
                                       name + " (TYPE postgres, READ_ONLY)");
          read = "postgres_query(" + QuoteLiteral(ExternalDatabaseName(key)) + ", sql)";
        }
        connections_.backend.Execute("CREATE MACRO memory.main." + name +
                                     "(sql) AS TABLE SELECT * FROM " + read);
      } catch (const BackendError& error) {
        return absl::InvalidArgumentError(error.what());
      }
      connections_.attached.insert(key);
    }
    return columns;
  }

 private:
  ExternalConnections& connections_;
  GcsClient gcs_;
};

}  // namespace

std::unique_ptr<ExternalTableProvider> NewServiceTableProvider(ExternalConnections& connections,
                                                               GcsClient gcs) {
  return std::make_unique<ServiceTableProvider>(connections, std::move(gcs));
}

}  // namespace bigquery_emulator_duckdb
