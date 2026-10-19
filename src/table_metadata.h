#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"
#include "src/field_schema.h"
#include "src/location.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {

// Table.timePartitioning on a column. Ingestion-time partitioning, which adds pseudo-columns,
// and partition expiration, which deletes data, are not emulated.
struct TimePartitioning {
  std::string type;  // DAY, HOUR, MONTH or YEAR.
  std::string field;

  bool operator==(const TimePartitioning&) const = default;
};

// Table.rangePartitioning: `field` split into partitions of `interval` from `start` to `end`.
struct RangePartitioning {
  std::string field;
  int64_t start = 0;
  int64_t end = 0;
  int64_t interval = 0;

  bool operator==(const RangePartitioning&) const = default;
};

// Table.cloneDefinition: the table a CREATE TABLE CLONE cloned, and when.
struct CloneDefinition {
  TableReference base_table;
  std::string clone_time;  // RFC 3339, as tables.get reports it.
};

// What a table or view carries besides its schema: Table.description, friendlyName and labels,
// and a table's partitioning and clustering, which only shape how BigQuery stores it, the
// defaultRoundingMode that NUMERIC and BIGNUMERIC fields added to it later take, the
// cloneDefinition of a clone, and its expiration. An empty description, friendly name or rounding
// mode is unset, as BigQuery reports it.
struct TableMetadata {
  std::string description;
  std::string friendly_name;
  std::map<std::string, std::string> labels;
  std::optional<TimePartitioning> time_partitioning;
  std::optional<RangePartitioning> range_partitioning;
  std::vector<std::string> clustering;  // Clustering.fields.
  std::string default_rounding_mode;    // ROUND_HALF_AWAY_FROM_ZERO or ROUND_HALF_EVEN.
  std::optional<CloneDefinition> clone;
  std::optional<int64_t> expiration_time;  // Milliseconds since the Unix epoch.
  // Output only and persisted with the table; the Storage API's default stream uses it.
  int64_t creation_time = 0;  // Milliseconds since the Unix epoch; unknown for old tables.

  [[nodiscard]] bool empty() const {
    return description.empty() && friendly_name.empty() && labels.empty() && !partitioned() &&
           clustering.empty() && default_rounding_mode.empty() && !clone.has_value() &&
           !expiration_time.has_value() && creation_time == 0;
  }
  [[nodiscard]] bool partitioned() const {
    return time_partitioning.has_value() || range_partitioning.has_value();
  }

  // The fields of a Table resource that it sets.
  [[nodiscard]] nlohmann::json ToJson() const;
};

// The schema and metadata of a table as tables.get reports them.
struct TableDescription {
  std::vector<FieldSchema> schema;
  TableMetadata metadata;
};

// A view as INFORMATION_SCHEMA describes it: its GoogleSQL query, empty when it is unknown, and
// its metadata.
struct ViewDescription {
  std::string query;
  TableMetadata metadata;
};

// What a dataset carries besides its reference: Dataset.description, friendlyName and labels,
// defaultTableExpirationMs, which sets the expiration of tables and views created in it later, and
// defaultRoundingMode, which tables created in it later take as their own.
// The emulator keeps them in a table of its own, since DuckDB cannot comment on a schema.
struct DatasetMetadata {
  std::string description;
  std::string friendly_name;
  std::map<std::string, std::string> labels;
  std::optional<int64_t> default_table_expiration_ms;
  std::string default_rounding_mode;  // ROUND_HALF_AWAY_FROM_ZERO or ROUND_HALF_EVEN.
  // Output only and recorded with the dataset.
  int64_t creation_time = 0;  // Milliseconds since the Unix epoch; unknown for old datasets.
  // As CanonicalLocation spells it; empty for a dataset recorded before datasets had locations,
  // which were all in the US.
  std::string location;

  [[nodiscard]] bool empty() const {
    return description.empty() && friendly_name.empty() && labels.empty() &&
           !default_table_expiration_ms.has_value() && default_rounding_mode.empty() &&
           creation_time == 0 && location.empty();
  }
  [[nodiscard]] std::string Location() const {
    return location.empty() ? kDefaultLocation : location;
  }

  // The fields of a Dataset resource that it sets.
  [[nodiscard]] nlohmann::json ToJson() const;
};

// The metadata of the Table resource `table`; a field it leaves out or sets to null is unset,
// and cloneDefinition, which is output only, is ignored. Throws ApiError::Invalid for a field of
// the wrong type, a label BigQuery rejects, or partitioning the emulator does not
// support.
TableMetadata TableMetadataFromJson(const nlohmann::json& table);

// tables.patch (`patch`) or tables.update applied to `metadata`. tables.patch replaces the fields
// `body` sets, clears those it sets to null, and merges its labels, a null value removing one;
// tables.update replaces the description, friendly name, labels, clustering, default rounding mode
// and expiration.
// Partitioning cannot change, so both keep it and reject a different one, and both keep the
// cloneDefinition. Throws as TableMetadataFromJson does.
void UpdateTableMetadata(TableMetadata& metadata, const nlohmann::json& body, bool patch);

// The metadata of the Dataset resource `dataset`, in the US unless it names another location. Any
// other field with a value besides those that identify the dataset or that the server reports is
// unsupported, since they change how BigQuery treats the dataset's tables. Throws
// ApiError::Invalid for those, for a field of the wrong type, for a location BigQuery does not
// have and for a label BigQuery rejects.
DatasetMetadata DatasetMetadataFromJson(const nlohmann::json& dataset);

// datasets.patch (`patch`) or datasets.update applied to `metadata`, as UpdateTableMetadata does
// for tables; a defaultTableExpirationMs of 0 clears it, as BigQuery documents for datasets.patch.
// A location must be the dataset's own, since a dataset cannot move. Throws as
// DatasetMetadataFromJson does.
void UpdateDatasetMetadata(DatasetMetadata& metadata, const nlohmann::json& body, bool patch);

// Throws ApiError::Invalid for a default table expiration BigQuery rejects: one shorter than an
// hour.
void ValidateDefaultTableExpiration(const std::optional<int64_t>& milliseconds);

// The expiration a table or view created at `creation_time` in a dataset with `dataset`'s default
// takes when its own `expiration_time` is unset; times are milliseconds since the Unix epoch.
std::optional<int64_t> ExpirationWithDefault(const std::optional<int64_t>& expiration_time,
                                             int64_t creation_time, const DatasetMetadata& dataset);

// Throws ApiError::Invalid for labels BigQuery rejects: more than 64, a key that is empty or
// does not start with a lowercase letter, or a key or value longer than 63 characters or with
// characters other than lowercase letters, digits, underscores and dashes. Characters outside
// ASCII are allowed.
void ValidateLabels(const std::map<std::string, std::string>& labels);

// Throws ApiError::Unsupported with `what` when `body` sets the EncryptionConfiguration `key`,
// since the emulator does not encrypt tables with Cloud KMS keys. Null or {} leaves it unset.
void RejectEncryptionConfiguration(const nlohmann::json& body, const char* key,
                                   const std::string& what);

// Throws ApiError::Invalid for partitioning or clustering that `schema` cannot have: a field it
// lacks, or of a type, mode or depth BigQuery rejects, more than 4 clustering fields, or a range
// that is empty or has more than 10,000 partitions.
void ValidatePartitioning(const TableMetadata& metadata, const std::vector<FieldSchema>& schema);

}  // namespace bigquery_emulator_duckdb
