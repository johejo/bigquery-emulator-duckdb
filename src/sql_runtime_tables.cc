#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "googlesql/public/strings.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/column_metadata.h"
#include "src/ddl_write.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/expiration_mutex.h"
#include "src/field_schema.h"
#include "src/function_catalog/catalog.h"
#include "src/location.h"
#include "src/partition_policy_history.h"
#include "src/partition_policy_read.h"
#include "src/references.h"
#include "src/schema_sql.h"
#include "src/sql_runtime.h"
#include "src/table_comments.h"
#include "src/table_history.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

std::vector<std::string> SqlRuntime::ListTables(DatasetReference dataset) {
  return ListTables(std::move(dataset), nullptr);
}

std::vector<std::string> SqlRuntime::ListTables(DatasetReference dataset,
                                                const ExpirationMutex::CatalogLock* catalog_lock) {
  dataset = ResolveDatasetReference(std::move(dataset));
  GetDataset(dataset);
  ExpireTables(backend_, expiration_mutex_, dataset, catalog_lock);
  return FirstColumnStrings(Execute(TablesQuery(dataset)));
}

std::vector<TableListEntry> SqlRuntime::ListTableEntries(DatasetReference dataset) {
  dataset = ResolveDatasetReference(std::move(dataset));
  GetDataset(dataset);
  ExpireTables(backend_, expiration_mutex_, dataset);
  std::vector<TableListEntry> entries;
  const QueryResult result = Execute(std::format(
      "SELECT table_name, type, comment FROM {} ORDER BY 1",
      LogicalCatalog(std::format("bq_relations(database := {}, schema := {})",
                                 QuoteLiteral(dataset.project_id),
                                 QuoteLiteral(DatasetCatalogName(dataset.dataset_id))))));
  for (size_t row = 0; row < result.rows.size(); ++row) {
    TableMetadata metadata = CommentMetadata(result.rows.OptionalString(row, 2));
    const auto view = ParseViewMetadata(result.rows.OptionalString(row, 2));
    entries.push_back({
        .table_id = result.rows.String(row, 0),
        .type = view && view->materialized             ? TableType::kMaterializedView
                : result.rows.String(row, 1) == "VIEW" ? TableType::kView
                : metadata.external_data               ? TableType::kExternal
                : metadata.snapshot.has_value()        ? TableType::kSnapshot
                                                       : TableType::kTable,
        .metadata = std::move(metadata),
    });
  }
  return entries;
}

std::string SqlRuntime::TableReadSnapshot::Relation() const {
  const auto& metadata = info.metadata;
  if (metadata.time_partitioning && !metadata.external_data && !metadata.clone &&
      !metadata.snapshot) {
    const auto query = PartitionPolicyTableReadQuery(
        *session, info.reference, evaluation_time_micros, PartitionRowVersion::kCurrent, true);
    if (!query.ok()) {
      throw ApiError::QueryFailure(std::string(query.status().message()));
    }
    return "(" + *query + ") AS _bq_visible_rows";
  }
  return ReadTableRelation(*session, info.reference);
}

TableInfo SqlRuntime::GetTable(TableReference table, bool include_row_count, bool expire) {
  return GetTable(std::move(table), include_row_count, expire, nullptr);
}

TableInfo SqlRuntime::GetTable(TableReference table, bool include_row_count, bool expire,
                               const ExpirationMutex::CatalogLock* catalog_lock) {
  auto read = BeginTableRead(std::move(table), expire, catalog_lock);
  if (include_row_count && !read.info.view_query && !read.info.metadata.external_data) {
    try {
      const QueryResult count = read.session->Execute("SELECT count(*) FROM " + read.Relation());
      read.info.num_rows = count.rows.Int64(0, 0);
    } catch (const BackendError& error) {
      throw ApiError::QueryFailure(error.what());
    }
  }
  read.session->Transaction("COMMIT");
  return std::move(read.info);
}

SqlRuntime::TableReadSnapshot SqlRuntime::BeginTableRead(
    TableReference table, bool expire, const ExpirationMutex::CatalogLock* catalog_lock) {
  table = ResolveTableReference(std::move(table));
  const DatasetReference dataset{.project_id = table.project_id, .dataset_id = table.dataset_id};
  GetDataset(dataset);
  // Relation expiration may mutate the catalog. Finish it before starting the read snapshot;
  // a read never takes a history mutex or writes retained rows/policies.
  if (expire) {
    ExpireTables(backend_, expiration_mutex_, dataset, catalog_lock);
  }
  TableReadSnapshot read{.session = backend_.NewSession()};
  Backend& session = *read.session;
  session.Transaction("BEGIN TRANSACTION");
  read.evaluation_time_micros = OpenPolicySnapshot(session);
  const auto location = LocationOf(session, dataset);
  if (!location) {
    throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" + dataset.dataset_id);
  }
  TableInfo& info = read.info;
  info.reference = table;
  info.location = *location;
  if (const auto comment = ViewComment(session, table); comment.has_value()) {
    std::optional<ViewMetadata> metadata = ParseViewMetadata(*comment);
    if (!metadata.has_value()) {
      throw ApiError::Invalid("View " + TableName(table) +
                              " was not created by the emulator and has no GoogleSQL definition");
    }
    info.materialized = metadata->materialized;
    info.materialized_source = std::move(metadata->materialized_source);
    info.view_query = std::move(metadata->query);
    info.schema = std::move(metadata->schema);
    info.metadata = std::move(metadata->metadata);
    return read;
  }
  try {
    info.schema = TableSchema(session, table);
    info.metadata = CommentMetadata(
        RelationComment(session, RelationType::kTable, table).value_or(std::nullopt));
    const QueryResult identity =
        session.Execute("SELECT table_oid FROM " +
                        RelationsQuery(table.project_id, table.dataset_id, table.table_id) +
                        " WHERE type = 'TABLE'");
    if (identity.rows.empty()) {
      throw ApiError::NotFound("Table not found: " + table.table_id);
    }
    info.storage_id = identity.rows.Int64(0, 0);
  } catch (const BackendError& error) {
    throw ApiError::NotFound("Not found: Table " + table.project_id + ":" + table.dataset_id + "." +
                             table.table_id);
  }
  return read;
}

std::vector<FieldSchema> SqlRuntime::DetectExternalSchema(const GcsExternalData& definition,
                                                          const std::string& table_id) {
  if (!external_tables_) {
    throw ApiError::Unsupported("external tables without an explicit schema");
  }
  std::vector<FieldSchema> schema = external_tables_(backend_)->DetectSchema(definition, table_id);
  SetModesGiven(schema);
  return schema;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a table's columns, then its keys.
std::vector<FieldSchema> SqlRuntime::ExternalSchema(GcsExternalData& definition,
                                                    std::vector<FieldSchema> schema,
                                                    const std::vector<FieldSchema>& declared_keys,
                                                    const std::string& table_id) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  if (schema.empty()) {
    schema = DetectExternalSchema(definition, table_id);
  }
  HivePartitioning* hive = definition.hive_partitioning ? &*definition.hive_partitioning : nullptr;
  if (hive == nullptr) {
    return schema;
  }
  hive->fields.clear();
  if (hive->mode.empty()) {
    return schema;
  }
  if (!external_tables_) {
    throw ApiError::Unsupported("Hive-partitioned external tables without an external provider");
  }
  const std::vector<FieldSchema> keys =
      external_tables_(backend_)->DetectPartitionKeys(definition, declared_keys, table_id);
  // The keys replace fields of the same names.
  std::erase_if(schema, [&keys](const FieldSchema& field) {
    return std::ranges::find(keys, field.name, &FieldSchema::name) != keys.end();
  });
  for (const FieldSchema& key : keys) {
    hive->fields.push_back(key.name);
    schema.push_back(key);
  }
  SetModesGiven(schema);
  return schema;
}

TableInfo SqlRuntime::CreateTable(TableReference table, std::vector<FieldSchema> schema,
                                  const TableMetadata& given) {
  table = ResolveTableReference(std::move(table), /*create=*/true);
  TableMetadata metadata = given;
  // Read the sources before holding the mutex, after the checks that fail without them.
  if (metadata.external_data && (schema.empty() || metadata.external_data->hive_partitioning)) {
    GetDataset(DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
    if (RelationComment(backend_, RelationType::kTable, table) || ViewComment(backend_, table)) {
      throw ApiError::Duplicate("Already Exists: Table " + table.project_id + ":" +
                                table.dataset_id + "." + table.table_id);
    }
    schema = ExternalSchema(*metadata.external_data, std::move(schema), {}, table.table_id);
  }
  const auto lock = expiration_mutex_.LockCatalog();
  table = ResolveTableReference(std::move(table), /*create=*/true);
  const DatasetMetadata dataset =
      GetDataset(DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  ExpireTables(backend_, expiration_mutex_,
               DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id},
               &lock);
  std::vector<std::string> columns;
  std::ranges::transform(schema, std::back_inserter(columns), [&](const FieldSchema& field) {
    return ColumnDefinition(field, functions_.timestamp_precision(),
                            metadata.external_data.has_value());
  });
  DdlWrite write = CreateTableWrite(
      TableDefinition{.table = table, .schema = schema, .metadata = metadata}, dataset);
  if (schema.empty()) {
    columns.push_back(QuoteIdentifier(kSchemaPlaceholderColumn) + " BOOLEAN");
    write.metadata_statements.push_back(std::format(
        "COMMENT ON COLUMN {}.{} IS {}", QualifiedName(table),
        QuoteIdentifier(kSchemaPlaceholderColumn), QuoteLiteral(kSchemaPlaceholderComment)));
  }
  auto defaults = schema;
  ApplyDefaultRoundingMode(defaults, metadata.default_rounding_mode == RoundingMode::kUnspecified
                                         ? dataset.default_rounding_mode
                                         : metadata.default_rounding_mode);
  std::ranges::move(ColumnDefaultStatements(table, defaults),
                    std::back_inserter(write.metadata_statements));
  try {
    backend_.ExecuteDdl("CREATE TABLE " + QualifiedName(table) + " (" + Join(columns, ", ") + ")",
                        write.metadata_statements, write.skip_query);
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Table " + table.project_id + ":" +
                                table.dataset_id + "." + table.table_id);
    }
    throw ApiError::Invalid(error.what());
  }
  return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
}

TableInfo SqlRuntime::CreateView(TableReference table, const json& definition,
                                 const TableMetadata& metadata, bool materialized) {
  const auto lock = expiration_mutex_.LockCatalog();
  table = ResolveTableReference(std::move(table), /*create=*/true);
  WriteView(table, definition, metadata, /*replace=*/false, materialized, lock);
  return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
}

TableInfo SqlRuntime::UpdateTable(TableReference table,
                                  const std::optional<std::vector<FieldSchema>>& schema,
                                  const std::optional<json>& view,
                                  const std::optional<TableMetadata>& metadata,
                                  bool rounding_mode_unspecified) {
  const auto lock = expiration_mutex_.LockCatalog();
  table = ResolveTableReference(std::move(table));
  const TableInfo info = GetTable(table, /*include_row_count=*/false, /*expire=*/true, &lock);
  if (info.view_query.has_value()) {
    if (metadata.has_value() && metadata->default_collation.has_value()) {
      throw ApiError::Unsupported("defaultCollation on a view");
    }
    if (schema.has_value()) {
      throw ApiError::Unsupported("changing the schema of a view; change its query instead");
    }
    if (view.has_value() && info.materialized) {
      throw ApiError::Invalid("A materialized view cannot be changed to a logical view");
    }
    if (view.has_value()) {
      // Every view the emulator keeps is GoogleSQL.
      json definition = {{"query", *info.view_query}, {"useLegacySql", false}};
      definition.update(*view);
      TableMetadata updated = metadata.value_or(info.metadata);
      updated.creation_time = info.metadata.creation_time;
      WriteView(table, definition, updated, /*replace=*/true, /*materialized=*/false, lock);
    } else if (metadata.has_value()) {
      TableMetadata updated = *metadata;
      updated.creation_time = info.metadata.creation_time;
      Execute(ViewCommentStatement(table, {
                                              .query = *info.view_query,
                                              .schema = info.schema,
                                              .metadata = updated,
                                              .materialized = info.materialized,
                                              .materialized_source = info.materialized_source,
                                          }));
    }
    return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
  }
  if (view.has_value()) {
    throw ApiError::Invalid("Table " + TableName(table) + " is not a view");
  }
  std::optional<std::vector<FieldSchema>> updated_schema = schema;
  // An external table keeps the schema it was created with, which a client may send back.
  if (info.metadata.external_data && schema.has_value()) {
    if (!SameSchema(*schema, info.schema)) {
      throw ApiError::Unsupported("changing the schema of an external table");
    }
    updated_schema.reset();
  }
  // A snapshot's schema, clustering and default rounding mode cannot change.
  if (info.metadata.snapshot.has_value() &&
      ((schema.has_value() && SchemaToJson(*schema) != SchemaToJson(info.schema)) ||
       (metadata.has_value() &&
        (metadata->clustering != info.metadata.clustering ||
         metadata->default_rounding_mode != info.metadata.default_rounding_mode ||
         metadata->default_collation != info.metadata.default_collation)))) {
    throw ApiError::Invalid(
        "Only table expirationTime, description, friendlyName, labels and tags can be updated for "
        "a snapshot.");
  }
  std::vector<std::string> statements;
  if (updated_schema.has_value()) {
    const RoundingMode mode = metadata.value_or(info.metadata).default_rounding_mode;
    ApplyDefaultRoundingMode(info.schema, *updated_schema, mode);
    ApplyDefaultCollation(info.schema, *updated_schema,
                          metadata.value_or(info.metadata).default_collation);
    ValidatePartitioning(metadata.value_or(info.metadata), *updated_schema);
    statements = SchemaUpdateStatements(table, info.schema, *updated_schema);
    if (info.schema.empty() && !updated_schema->empty()) {
      statements.push_back(std::format("ALTER TABLE {} DROP COLUMN {}", QualifiedName(table),
                                       QuoteIdentifier(kSchemaPlaceholderColumn)));
    }
    // The rows kept from before the table had these columns are not those it has now.
    if (const int64_t creation_time = HistoryCreationTime(table, info.metadata);
        !statements.empty() && creation_time != 0) {
      std::ranges::move(DiscardHistoryStatements(table, creation_time),
                        std::back_inserter(statements));
      statements.push_back(CommitVersionsStatement(table.project_id));
    }
    std::vector<FieldSchema> defaults;
    for (size_t i = 0; i < info.schema.size(); ++i) {
      const FieldSchema& before = info.schema.at(i);
      const FieldSchema& after = updated_schema->at(i);
      if (after.default_value_expression == before.default_value_expression) {
        continue;
      }
      if (std::ranges::any_of(info.metadata.primary_key, [&](const std::string& name) {
            return ToLowerAscii(name) == ToLowerAscii(before.name);
          })) {
        throw ApiError::Invalid(std::format(
            "Key column {} cannot be modified or removed. column's default_value_expression "
            "changed: {} -> {}",
            before.name, before.default_value_expression, after.default_value_expression));
      }
      if (after.default_value_expression.empty()) {
        statements.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} DROP DEFAULT",
                                         QualifiedName(table), QuoteIdentifier(after.name)));
        // REPEATED columns without a declared default still default to an empty array.
        std::ranges::move(RepeatedColumnDefaultStatements(table, {after}),
                          std::back_inserter(statements));
      } else {
        defaults.push_back(after);
      }
    }
    std::ranges::move(ColumnDefaultStatements(table, defaults), std::back_inserter(statements));
    std::ranges::move(ColumnCommentStatements(table, *updated_schema),
                      std::back_inserter(statements));
  }
  if (metadata.has_value()) {
    TableMetadata updated = *metadata;
    updated.creation_time = info.metadata.creation_time;
    // UNSPECIFIED leaves the stored default intact, but newly added fields above take no mode.
    if (rounding_mode_unspecified) {
      updated.default_rounding_mode = info.metadata.default_rounding_mode;
    }
    if (updated.time_partitioning && info.metadata.time_partitioning &&
        updated.time_partitioning->expiration_ms !=
            info.metadata.time_partitioning->expiration_ms) {
      statements.push_back(
          KeepPartitionPolicyStatement(table, updated.time_partitioning->expiration_ms));
      statements.push_back(CommitPartitionPoliciesStatement(table.project_id));
    }
    statements.push_back(
        TableCommentStatement(table, updated, updated_schema.value_or(info.schema)));
  }
  if (statements.empty()) {
    return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
  }
  try {
    backend_.ExecuteDdl(statements.front(), {statements.begin() + 1, statements.end()}, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
  return GetTable(table, /*include_row_count=*/true, /*expire=*/false);
}

void SqlRuntime::WriteView(TableReference table, const json& definition,
                           const TableMetadata& metadata, bool replace, bool materialized,
                           const ExpirationMutex::CatalogLock& catalog_lock) {
  table = ResolveTableReference(std::move(table), /*create=*/true);
  const DatasetMetadata dataset =
      GetDataset(DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id});
  if (!definition.is_object()) {
    throw ApiError::Invalid("Invalid view definition");
  }
  if (materialized) {
    for (const auto& [name, value] : definition.items()) {
      if (name == "lastRefreshTime" || name == "refreshWatermark") {
        continue;  // Output-only fields.
      }
      if ((name == "enableRefresh" && value == true) ||
          (name == "allowNonIncrementalDefinition" && value == false)) {
        continue;
      }
      if (name != "query") {
        throw ApiError::Unsupported("materializedView field " + name);
      }
    }
  }
  if (!materialized && definition.value("useLegacySql", true)) {
    throw ApiError::Unsupported("legacy SQL views");
  }
  if (definition.contains("userDefinedFunctionResources")) {
    throw ApiError::Unsupported("view userDefinedFunctionResources");
  }
  const std::string query = definition.value("query", "");
  if (query.empty()) {
    throw ApiError::Invalid("View query is required");
  }
  // A view reads only tables in its own dataset's location.
  JobLocation location(dataset.Location());
  try {
    const TranslatedStatement translation =
        Translate(std::string(materialized ? "CREATE MATERIALIZED VIEW "
                              : replace    ? "CREATE OR REPLACE VIEW "
                                           : "CREATE VIEW ") +
                      googlesql::ToIdentifierLiteral(table.project_id + "." + table.dataset_id +
                                                     "." + table.table_id) +
                      " AS " + query,
                  {}, table.project_id, "", &location, nullptr, /*stores_result=*/false,
                  /*dry_run=*/false, nullptr, &catalog_lock);
    if (!translation.view.has_value()) {
      throw ApiError::Internal("CREATE VIEW was not translated to a view");
    }
    ViewDefinition view = *translation.view;
    // REST-created views report explicit modes, unlike views created through DDL.
    SetModesGiven(view.schema);
    view.metadata = metadata;
    // A replaced view keeps the expiration `metadata` gives it.
    backend_.ExecuteDdl(
        translation.sql,
        CreateViewWrite(view, replace ? DatasetMetadata{} : dataset).metadata_statements, "");
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Table " + TableName(table));
    }
    throw ApiError::Invalid(error.what());
  } catch (const std::runtime_error& error) {
    throw ApiError::Invalid(error.what());
  }
}

int64_t SqlRuntime::HistoryCreationTime(const TableReference& table,
                                        const TableMetadata& metadata) const {
  return project_ids_.contains(table.project_id) ? metadata.creation_time : 0;
}

absl::Mutex& SqlRuntime::HistoryMutex(const TableReference& table) {
  if (project_ids_.contains(table.project_id)) {
    SweepHistory(backend_, table.project_id);
  }
  const absl::MutexLock lock(history_mutexes_mutex_);
  std::unique_ptr<absl::Mutex>& mutex = history_mutexes_[ToLowerAscii(QualifiedName(table))];
  if (!mutex) {
    mutex = std::make_unique<absl::Mutex>();
  }
  return *mutex;
}

std::unique_lock<absl::Mutex> SqlRuntime::KeepVersion(Backend& backend,
                                                      const TableReference& table) {
  std::unique_lock<absl::Mutex> lock;
  if (!backend.InTransaction()) {
    lock = std::unique_lock(HistoryMutex(table));
  }
  const auto comment = RelationComment(backend, RelationType::kTable, table);
  const int64_t creation_time =
      comment.has_value() ? HistoryCreationTime(table, CommentMetadata(*comment)) : 0;
  if (creation_time == 0) {
    return lock;
  }
  const std::vector<std::string> statements = KeepVersionStatements(table, creation_time);
  if (backend.InTransaction()) {
    backend.ExecuteAll(statements);
    backend.BeforeCommit(CommitVersionsStatement(table.project_id));
    return lock;
  }
  const auto session = backend_.NewSession();
  session->Transaction("BEGIN TRANSACTION");
  session->ExecuteAll(statements);
  session->BeforeCommit(CommitVersionsStatement(table.project_id));
  session->Transaction("COMMIT");
  return lock;
}

void RejectReadOnlyWrite(const TableInfo& table) {
  if (table.metadata.external_data) {
    throw ApiError::Invalid("Cannot write to external table " + TableName(table.reference));
  }
  if (table.metadata.snapshot.has_value()) {
    throw ApiError::Invalid("Table " + TableName(table.reference) +
                            " is a snapshot, and snapshots are immutable.");
  }
}

void SqlRuntime::DeleteTable(TableReference table) {
  const auto lock = expiration_mutex_.LockCatalog();
  table = ResolveTableReference(std::move(table));
  ExpireTables(backend_, expiration_mutex_,
               DatasetReference{.project_id = table.project_id, .dataset_id = table.dataset_id},
               &lock);
  // Views the emulator did not create have no metadata for GetTable, but can still be dropped.
  if (ViewComment(backend_, table).has_value()) {
    Execute("DROP VIEW " + QualifiedName(table));
    return;
  }
  GetTable(table, /*include_row_count=*/false, /*expire=*/true, &lock);
  Execute("DROP TABLE " + QualifiedName(table));
}

}  // namespace bigquery_emulator_duckdb
