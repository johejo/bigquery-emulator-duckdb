#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"
#include "src/field_schema.h"
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
// and a table's partitioning and clustering, which only shape how BigQuery stores it, and the
// cloneDefinition of a clone, and its expiration. An empty description or friendly name is unset,
// as BigQuery reports it.
struct TableMetadata {
  std::string description;
  std::string friendly_name;
  std::map<std::string, std::string> labels;
  std::optional<TimePartitioning> time_partitioning;
  std::optional<RangePartitioning> range_partitioning;
  std::vector<std::string> clustering;  // Clustering.fields.
  std::optional<CloneDefinition> clone;
  std::optional<int64_t> expiration_time;  // Milliseconds since the Unix epoch.

  [[nodiscard]] bool empty() const {
    return description.empty() && friendly_name.empty() && labels.empty() && !partitioned() &&
           clustering.empty() && !clone.has_value() && !expiration_time.has_value();
  }
  [[nodiscard]] bool partitioned() const {
    return time_partitioning.has_value() || range_partitioning.has_value();
  }

  // The fields of a Table resource that it sets.
  [[nodiscard]] nlohmann::json ToJson() const;
};

// What a dataset carries besides its reference: Dataset.description, friendlyName and labels. The
// emulator keeps them in a table of its own, since DuckDB cannot comment on a schema.
struct DatasetMetadata {
  std::string description;
  std::string friendly_name;
  std::map<std::string, std::string> labels;

  [[nodiscard]] bool empty() const {
    return description.empty() && friendly_name.empty() && labels.empty();
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
// tables.update replaces the description, friendly name, labels, clustering and expiration.
// Partitioning cannot change, so both keep it and reject a different one, and both keep the
// cloneDefinition. Throws as TableMetadataFromJson does.
void UpdateTableMetadata(TableMetadata& metadata, const nlohmann::json& body, bool patch);

// The metadata of the Dataset resource `dataset`. Every dataset is in the US, so a location
// elsewhere is unsupported, and so is any other field with a value besides those that identify
// the dataset or that the server reports, since they change how BigQuery treats the dataset's
// tables. Throws ApiError::Invalid for those, for a field of the wrong type and for a label
// BigQuery rejects.
DatasetMetadata DatasetMetadataFromJson(const nlohmann::json& dataset);

// datasets.patch (`patch`) or datasets.update applied to `metadata`, as UpdateTableMetadata does
// for tables. Throws as DatasetMetadataFromJson does.
void UpdateDatasetMetadata(DatasetMetadata& metadata, const nlohmann::json& body, bool patch);

// Throws ApiError::Invalid for labels BigQuery rejects: more than 64, a key that is empty or
// does not start with a lowercase letter, or a key or value longer than 63 characters or with
// characters other than lowercase letters, digits, underscores and dashes. Characters outside
// ASCII are allowed.
void ValidateLabels(const std::map<std::string, std::string>& labels);

// Throws ApiError::Invalid for partitioning or clustering that `schema` cannot have: a field it
// lacks, or of a type, mode or depth BigQuery rejects, more than 4 clustering fields, or a range
// that is empty or has more than 10,000 partitions.
void ValidatePartitioning(const TableMetadata& metadata, const std::vector<FieldSchema>& schema);

}  // namespace bigquery_emulator_duckdb
