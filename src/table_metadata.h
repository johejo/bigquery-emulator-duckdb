#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json_fwd.hpp"
#include "src/field_schema.h"
#include "src/location.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {

enum class TimePartitionUnit : uint8_t { kDay, kHour, kMonth, kYear };
std::string_view TimePartitionUnitName(TimePartitionUnit unit);
std::optional<TimePartitionUnit> ParseTimePartitionUnit(std::string_view name);

// Table.timePartitioning on a column. Ingestion-time partitioning, which adds pseudo-columns,
// and partition expiration, which deletes data, are not emulated.
struct TimePartitioning {
  TimePartitionUnit type = TimePartitionUnit::kDay;
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

// Table.cloneDefinition or snapshotDefinition: the table a clone or a table snapshot was made
// of, and when.
struct BaseTableDefinition {
  TableReference base_table;
  // cloneTime or snapshotTime in RFC 3339, as tables.get reports it; empty until the table is
  // created, which sets it to the table's creation time.
  std::string time = {};
};

// What a table or view carries besides its schema: Table.description, friendlyName, labels and
// resourceTags, a table's unenforced primary key, its partitioning and clustering, which only
// shape how BigQuery stores it, the defaultRoundingMode that NUMERIC and BIGNUMERIC fields added
// to it later take, the cloneDefinition of a clone or snapshotDefinition of a table snapshot, and
// its expiration. An empty description or friendly name, or kUnspecified rounding mode, is unset.
struct TableMetadata {
  std::string description;
  std::string friendly_name;
  std::map<std::string, std::string> labels;
  // DDL array order, which TABLE_OPTIONS exposes.
  std::vector<std::string> label_order;
  std::map<std::string, std::string> resource_tags;
  std::vector<std::string> tag_order;
  std::optional<TimePartitioning> time_partitioning;
  std::optional<RangePartitioning> range_partitioning;
  // Queries must have a predicate eligible for partition elimination.
  bool require_partition_filter = false;
  std::vector<std::string> clustering;  // Clustering.fields.
  // TableConstraints.primaryKey.columns; empty when the table has no primary key.
  std::vector<std::string> primary_key;
  RoundingMode default_rounding_mode = RoundingMode::kUnspecified;
  std::optional<BaseTableDefinition> clone;
  // A table snapshot is read-only: only its metadata can change.
  std::optional<BaseTableDefinition> snapshot;
  // Restored tables list friendly_name and labels in TABLE_OPTIONS even when empty.
  bool restored_from_snapshot = false;
  std::optional<int64_t> expiration_time;  // Milliseconds since the Unix epoch.
  // Output only and persisted with the table; the Storage API's default stream uses it.
  int64_t creation_time = 0;  // Milliseconds since the Unix epoch; unknown for old tables.

  [[nodiscard]] bool empty() const {
    return description.empty() && friendly_name.empty() && labels.empty() &&
           resource_tags.empty() && !partitioned() && !require_partition_filter &&
           clustering.empty() && primary_key.empty() &&
           default_rounding_mode == RoundingMode::kUnspecified && !clone.has_value() &&
           !snapshot.has_value() && !expiration_time.has_value() && !restored_from_snapshot &&
           creation_time == 0;
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

// The identity of the base table of a materialized view. Recreating the table does not
// repair the materialized view; its original creation time must still match.
struct MaterializedViewSource {
  TableReference table;
  int64_t creation_time = 0;
};

// A view as INFORMATION_SCHEMA describes it: its GoogleSQL query, empty when it is unknown, and
// its metadata.
struct ViewDescription {
  std::string query;
  TableMetadata metadata;
  bool materialized = false;
  std::optional<MaterializedViewSource> materialized_source = {};
};

// What a dataset carries besides its reference: Dataset.description, friendlyName, labels and
// resourceTags, defaultTableExpirationMs, which sets the expiration of tables and views created in
// it later, and defaultRoundingMode, which tables created in it later take as their own. The
// emulator keeps them in a table of its own, since DuckDB cannot comment on a schema.
struct DatasetMetadata {
  std::string description;
  std::string friendly_name;
  std::map<std::string, std::string> labels;
  std::vector<std::string> label_order;
  std::map<std::string, std::string> resource_tags;
  std::vector<std::string> tag_order;
  std::optional<int64_t> default_table_expiration_ms;
  RoundingMode default_rounding_mode = RoundingMode::kUnspecified;
  // Output only and recorded with the dataset.
  int64_t creation_time = 0;  // Milliseconds since the Unix epoch; unknown for old datasets.
  // As CanonicalLocation spells it; empty for a dataset recorded before datasets had locations,
  // which were all in the US.
  std::string location;

  [[nodiscard]] bool empty() const {
    return description.empty() && friendly_name.empty() && labels.empty() &&
           resource_tags.empty() && !default_table_expiration_ms.has_value() &&
           default_rounding_mode == RoundingMode::kUnspecified && creation_time == 0 &&
           location.empty();
  }
  [[nodiscard]] std::string Location() const {
    return location.empty() ? kDefaultLocation : location;
  }

  // The fields of a Dataset resource that it sets.
  [[nodiscard]] nlohmann::json ToJson() const;
};

// The metadata of the Table resource `table`; a field it leaves out or sets to null is unset,
// and cloneDefinition and snapshotDefinition, which are output only, are ignored. Throws
// ApiError::Invalid for a field of the wrong type, a label BigQuery rejects, or partitioning the
// emulator does not support.
TableMetadata TableMetadataFromJson(const nlohmann::json& table);

// tables.patch (`patch`) or tables.update applied to `metadata`. tables.patch replaces the fields
// `body` sets, clears those it sets to null, labels included, and merges its labels, a null value
// removing one;
// resourceTags patches merge keys and preserve their order. An empty object is a no-op on
// tables, and null clears table tags. Empty patches on tagged datasets and null dataset tags
// remain unsupported. Full updates replace resourceTags.
// tables.update replaces the description, friendly name, labels, clustering, primary key, default
// rounding mode and expiration, including the required partition filter. tables.patch keeps the
// primary key when tableConstraints has none. Both drop the foreign keys of tableConstraints, as
// DDL does.
// Partitioning cannot change, so both keep it and reject a different one, and both keep the
// cloneDefinition and snapshotDefinition. Throws as TableMetadataFromJson does.
void UpdateTableMetadata(TableMetadata& metadata, const nlohmann::json& body, bool patch);

// The metadata of the Dataset resource `dataset`, in the US unless it names another location.
// Throws ApiError::Invalid for a field of the wrong type, for a location BigQuery does not have
// and for a label BigQuery rejects.
DatasetMetadata DatasetMetadataFromJson(const nlohmann::json& dataset);

// datasets.patch (`patch`) or datasets.update applied to `metadata`, as UpdateTableMetadata does
// for tables, except that datasets.update keeps the friendly name when `body` leaves it out; a
// defaultTableExpirationMs of 0 clears it, as BigQuery documents for datasets.patch. A location
// must be the dataset's own, since a dataset cannot move. Throws as DatasetMetadataFromJson does.
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
// Checks locally verifiable tag limits; Cloud Resource Manager definitions are not consulted.
void ValidateResourceTags(const std::map<std::string, std::string>& tags);

// Throws ApiError::Unsupported with `what` when `body` sets the EncryptionConfiguration `key`,
// since the emulator does not encrypt tables with Cloud KMS keys. Null or {} leaves it unset.
void RejectEncryptionConfiguration(const nlohmann::json& body, const char* key,
                                   const std::string& what);

// The columns of `primary_key` as `schema` spells them, which BigQuery reports whatever case
// they are given in. Throws ApiError::Invalid for a column `schema` lacks or of a type BigQuery
// rejects. BigQuery takes a REST primary key that names a column twice. BigQuery's documentation
// says that primary key columns cannot have default values, but it creates such tables.
std::vector<std::string> PrimaryKeyColumns(const std::vector<std::string>& primary_key,
                                           const std::vector<FieldSchema>& schema);

// Throws ApiError::Invalid for partitioning or clustering that `schema` cannot have: a field it
// lacks, or of a type, mode or depth BigQuery rejects, more than 4 clustering fields, or a range
// that is empty or has more than 10,000 partitions.
void ValidatePartitioning(const TableMetadata& metadata, const std::vector<FieldSchema>& schema);

// DuckDB cannot comment on a schema, so the emulator records each dataset's DatasetMetadata as
// JSON in this table of the project's `main` schema, which is not a dataset. A dataset the table
// has no row for has no metadata. Writes to it share the transaction that creates or drops the
// dataset's schema.
std::string DatasetMetadataTable(const std::string& project);

}  // namespace bigquery_emulator_duckdb
