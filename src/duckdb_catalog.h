#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/field_schema.h"
#include "src/references.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

std::string PropertyGraphsTable(const std::string& project);
std::string PropertyGraphQuery(const TableReference& graph);

class Backend;
struct QueryResult;

// Reads the BigQuery metadata recorded in DuckDB through the supplied backend, including a
// script session's uncommitted changes. These helpers do not resolve project aliases.
std::vector<std::string> FirstColumnStrings(const QueryResult& result);
bool IsDuckDbSchema(const std::string& name);
std::string DatasetsQuery(const std::string& project);
std::string DatasetEntriesQuery(const std::string& project,
                                const std::optional<std::string>& dataset = std::nullopt);
DatasetMetadata ParseDatasetMetadata(const std::optional<std::string>& recorded);
std::string TablesQuery(const DatasetReference& dataset);

// Lazy expiration shares the emulator's mutex with mutations. Each drop rechecks the comment
// in its transaction so a concurrent replacement or extended expiration is preserved.
void ExpireTables(Backend& backend, std::recursive_mutex& expiration_mutex,
                  const std::optional<DatasetReference>& dataset = std::nullopt);

// An absent relation returns nullopt; a relation without a comment returns an empty comment.
std::optional<std::optional<std::string>> RelationComment(Backend& backend,
                                                          std::string_view relations,
                                                          std::string_view name,
                                                          const TableReference& table);
std::optional<std::optional<std::string>> ViewComment(Backend& backend,
                                                      const TableReference& table);

// Restores view and column metadata without requiring a view's execution identity.
std::vector<FieldSchema> TableSchema(Backend& backend, const TableReference& table);
// The table as a FROM relation, converting external numeric columns when needed.
std::string ReadTableRelation(Backend& backend, const TableReference& table);
// Rejects writes whose query values would be interpreted as a different physical number.
void CheckWritableTable(Backend& backend, const TableReference& table);

std::optional<std::string> LocationOf(Backend& backend, const DatasetReference& dataset);

}  // namespace bigquery_emulator_duckdb
