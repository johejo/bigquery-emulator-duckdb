#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "src/expiration_mutex.h"
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
// Resolve resource names according to the dataset's case-sensitivity setting before
// encoding their DuckDB identifiers. Returned references retain the stored spelling.
std::optional<DatasetReference> FindDatasetReference(Backend& backend,
                                                     const DatasetReference& dataset);
std::optional<TableReference> FindTableReference(Backend& backend, const TableReference& table);
// Unicode identifier case folding remains unsupported; do not silently use DuckDB's rules.
void CheckCaseInsensitiveTableName(Backend& backend, const TableReference& table);
void CheckCaseInsensitiveDataset(Backend& backend, const DatasetReference& dataset,
                                 const DatasetMetadata& metadata);
std::string TablesQuery(const DatasetReference& dataset);
// Whether any stored foreign key references this table, across attached projects and datasets.
// Used to reject destructive metadata changes whose effect on foreign keys is unverified.
bool HasForeignKeyReferences(Backend& backend, const TableReference& table);
// Metadata changes to run in the transaction that drops a table or its primary key.
std::vector<std::string> DropForeignKeyReferenceStatements(Backend& backend,
                                                           const TableReference& table);
// References from tables outside a dataset must not be silently changed by deleting it.
bool HasOutsideForeignKeyReferences(Backend& backend, const DatasetReference& dataset);

// Lazy expiration shares the emulator's mutex with mutations. Each drop rechecks the comment
// in its transaction so a concurrent replacement or extended expiration is preserved. A caller
// already holding the catalog lock passes it so drops do not acquire it again.
void ExpireTables(Backend& backend, ExpirationMutex& expiration_mutex,
                  const std::optional<DatasetReference>& dataset = std::nullopt,
                  const ExpirationMutex::CatalogLock* catalog_lock = nullptr);

enum class RelationType : std::uint8_t { kTable, kView };

// The comment of `table`, a table or view as `type` says. An absent relation returns nullopt; a
// relation without a comment returns an empty comment.
std::optional<std::optional<std::string>> RelationComment(Backend& backend, RelationType type,
                                                          const TableReference& table);
std::optional<std::optional<std::string>> ViewComment(Backend& backend,
                                                      const TableReference& table);

// Restores view and column metadata without requiring a view's execution identity.
std::vector<FieldSchema> TableSchema(Backend& backend, const TableReference& requested);
// The same metadata read when the caller has already resolved the stored spelling.
std::vector<FieldSchema> ResolvedTableSchema(Backend& backend, const TableReference& table);
// An alternative query of the table's rows, or nullopt for its named relation.
std::optional<std::string> TableReadQuery(Backend& backend, const TableReference& table);
// Restrictions on ordinary row writes, independent of the table's read query.
TableWriteRestriction TableWriteRestrictionOf(Backend& backend, const TableReference& table);
// The table as a FROM relation, using its alternative read query when present.
std::string ReadTableRelation(Backend& backend, const TableReference& table);
// Rejects ordinary row writes to restricted storage, including snapshots.
void CheckWritableTable(Backend& backend, const TableReference& table);

std::optional<std::string> LocationOf(Backend& backend, const DatasetReference& dataset);

}  // namespace bigquery_emulator_duckdb
