#pragma once

#include <map>
#include <string>

#include "nlohmann/json_fwd.hpp"

namespace bigquery_emulator_duckdb {

// What a table or view carries besides its schema: Table.description, friendlyName and labels.
// An empty description or friendly name is unset, as BigQuery reports it.
struct TableMetadata {
  std::string description;
  std::string friendly_name;
  std::map<std::string, std::string> labels;

  bool empty() const { return description.empty() && friendly_name.empty() && labels.empty(); }

  // The fields of a Table resource that it sets.
  nlohmann::json ToJson() const;
};

// The metadata of the Table resource `table`; a field it leaves out or sets to null is unset.
// Throws ApiError::Invalid for a field of the wrong type or a label BigQuery rejects.
TableMetadata TableMetadataFromJson(const nlohmann::json& table);

// tables.patch: replaces the fields `patch` sets, clears those it sets to null, and merges its
// labels, a null value removing one. Throws as TableMetadataFromJson does.
void PatchTableMetadata(TableMetadata& metadata, const nlohmann::json& patch);

// Throws ApiError::Invalid for labels BigQuery rejects: more than 64, a key that is empty or
// does not start with a lowercase letter, or a key or value longer than 63 characters or with
// characters other than lowercase letters, digits, underscores and dashes. Characters outside
// ASCII are allowed.
void ValidateLabels(const std::map<std::string, std::string>& labels);

}  // namespace bigquery_emulator_duckdb
