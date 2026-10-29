#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "src/api_error.h"
#include "src/backend_error.h"
#include "src/ddl_write.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/location.h"
#include "src/references.h"
#include "src/schema_sql.h"
#include "src/sql_runtime.h"
#include "src/table_history.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {
namespace {

// The field of `schema` named `name`, which BigQuery compares without case.
std::vector<FieldSchema>::const_iterator FindField(const std::vector<FieldSchema>& schema,
                                                   const std::string& name) {
  return std::ranges::find_if(schema, [&](const FieldSchema& field) {
    return std::ranges::equal(field.name, name, [](unsigned char a, unsigned char b) {
      return std::tolower(a) == std::tolower(b);
    });
  });
}

}  // namespace

void SqlRuntime::UseDataset(JobLocation& location, const TableReference& table) {
  const DatasetReference dataset{.project_id = table.project_id, .dataset_id = table.dataset_id};
  if (const std::optional<std::string> found = DatasetLocation(backend_, dataset)) {
    location.Reference("Dataset " + dataset.project_id + ":" + dataset.dataset_id, *found);
  }
}

std::string_view DispositionName(CreateDisposition disposition) {
  switch (disposition) {
    case CreateDisposition::kCreateIfNeeded:
      return "CREATE_IF_NEEDED";
    case CreateDisposition::kCreateNever:
      return "CREATE_NEVER";
  }
  return "";
}

std::string_view DispositionName(WriteDisposition disposition) {
  switch (disposition) {
    case WriteDisposition::kWriteEmpty:
      return "WRITE_EMPTY";
    case WriteDisposition::kWriteAppend:
      return "WRITE_APPEND";
    case WriteDisposition::kWriteTruncate:
      return "WRITE_TRUNCATE";
    case WriteDisposition::kWriteTruncateData:
      return "WRITE_TRUNCATE_DATA";
  }
  return "";
}

std::optional<CreateDisposition> ParseCreateDisposition(std::string_view name) {
  for (const CreateDisposition disposition :
       {CreateDisposition::kCreateIfNeeded, CreateDisposition::kCreateNever}) {
    if (DispositionName(disposition) == name) {
      return disposition;
    }
  }
  return std::nullopt;
}

std::optional<WriteDisposition> ParseWriteDisposition(std::string_view name) {
  for (const WriteDisposition disposition : {
           WriteDisposition::kWriteEmpty,
           WriteDisposition::kWriteAppend,
           WriteDisposition::kWriteTruncate,
           WriteDisposition::kWriteTruncateData,
       }) {
    if (DispositionName(disposition) == name) {
      return disposition;
    }
  }
  return std::nullopt;
}

QueryResult SqlRuntime::WriteDestination(const std::string& project_id, TableReference destination,
                                         CreateDisposition create, WriteDisposition write,
                                         const std::string& sql,
                                         const std::vector<FieldSchema>& schema,
                                         const std::vector<std::string>& setup, bool count_only,
                                         const TableMetadata& metadata, bool copy, bool restore) {
  std::set<std::string> names;
  std::vector<std::string> duplicates;
  for (const FieldSchema& field : schema) {
    std::string name = field.name;
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!names.insert(name).second) {
      duplicates.push_back(field.name);
    }
  }
  if (!duplicates.empty()) {
    throw ApiError::InvalidQuery(
        "Duplicate column names in the result are not supported. Found duplicate(s): " +
        Join(duplicates, ", "));
  }

  if (destination.project_id.empty()) {
    destination.project_id = project_id;
  }
  destination.project_id = ResolveProject(destination.project_id);
  const DatasetMetadata dataset = GetDataset(
      DatasetReference{.project_id = destination.project_id, .dataset_id = destination.dataset_id});
  std::optional<TableInfo> existing;
  try {
    existing = GetTable(destination);
  } catch (const ApiError& error) {
    if (error.http_status() != 404) {
      throw;
    }
  }
  if (existing.has_value() && existing->view_query) {
    throw ApiError::Invalid("Cannot write to a view: " + TableName(destination));
  }
  if (existing.has_value()) {
    RejectReadOnlyWrite(*existing);
  }
  if (!existing.has_value() && create == CreateDisposition::kCreateNever) {
    throw ApiError::NotFound("Not found: Table " + TableName(destination));
  }
  if (existing.has_value() && write == WriteDisposition::kWriteEmpty && existing->num_rows > 0) {
    throw ApiError::Duplicate("Already Exists: Table " + TableName(destination));
  }
  if (existing.has_value() && write != WriteDisposition::kWriteTruncate) {
    CheckWritableTable(backend_, destination);
  }
  // Existing destinations retain their write constraints. Appended rows are written with
  // DuckDB's casts, which round halfway values away from zero, so
  // the values of ROUND_HALF_EVEN fields are rounded to even first, as BigQuery rounds those a
  // query or a load appends. A copy from a table of the same schema has nothing to round, and
  // BigQuery rejects a copy from another.
  std::string appended = "*";
  if (existing.has_value() && write != WriteDisposition::kWriteTruncate) {
    if (copy && HasRoundHalfEven(existing->schema) && !SameSchema(existing->schema, schema)) {
      throw ApiError::Invalid("Source and destination tables have different schemas");
    }
    std::vector<std::string> columns;
    for (const FieldSchema& field : schema) {
      const std::string name = QuoteIdentifier(field.name);
      const auto column = FindField(existing->schema, field.name);
      const std::optional<std::string> value =
          column == existing->schema.end()
              ? name
              : TranslateAppendedValue(functions_, name, field, *column);
      if (!value.has_value()) {
        throw ApiError::Unsupported("appending rows of another schema to ROUND_HALF_EVEN columns");
      }
      columns.push_back(*value + " AS " + name);
    }
    appended = Join(columns, ", ");
  }
  // Whether BigQuery replaces an existing table's metadata, or keeps it, is not known, besides the
  // primary key a copy gives the table it truncates.
  TableMetadata replaced = metadata;
  replaced.primary_key.clear();
  if (existing.has_value() && !replaced.empty()) {
    throw ApiError::Unsupported("setting the expiration of an existing destination table " +
                                TableName(destination));
  }

  // The result is materialized first, which runs the query once even when it reads the
  // destination itself. DuckDB lets a transaction write to a single database, so the temporary
  // table is filled before the transaction that writes the destination begins.
  const std::string result_table = "temp.main._bigquery_emulator_query_result";
  std::vector<std::string> aliases;
  std::ranges::transform(schema, std::back_inserter(aliases),
                         [&](const FieldSchema& field) { return QuoteIdentifier(field.name); });
  const std::string target = QualifiedName(destination);
  std::vector<std::string> statements = {
      std::format("CREATE TEMP TABLE _bigquery_emulator_query_result AS"
                  " SELECT * FROM ({}) AS _bigquery_emulator_query_result({})",
                  sql, Join(aliases, ", ")),
      "BEGIN TRANSACTION",
  };
  // Holds other writes to the destination until this one, which keeps its rows first, commits.
  std::unique_lock<std::mutex> kept;
  if (!existing.has_value() || write == WriteDisposition::kWriteTruncate) {
    if (existing.has_value()) {
      statements.push_back("DROP TABLE " + target);
    }
    // The columns take BigQuery's types for the result, so the table reads back as the query's
    // schema rather than as whatever DuckDB computed.
    if (const std::optional<std::string> columns = ColumnDefinitions(schema)) {
      statements.push_back(std::format("CREATE TABLE {} ({})", target, *columns));
    } else {
      statements.push_back("CREATE TABLE " + target + " AS SELECT * FROM " + result_table +
                           " LIMIT 0");
    }
    // Creation and its schema, defaults and table metadata commit together, as for DDL and REST.
    // The job's disposition decides what to replace; CreateTableWrite only initializes the table.
    // Only a table the job creates takes the dataset's default expiration, which BigQuery
    // documents for new tables. As BigQuery answers, a replaced table keeps its default rounding
    // mode, and a copy job's new table takes no default
    // rounding mode, neither the source's nor the dataset's.
    if ((copy &&
         (metadata.default_collation.has_value() || dataset.default_collation.has_value())) ||
        (existing.has_value() && existing->metadata.default_collation.has_value())) {
      throw ApiError::Unsupported("copying or replacing a destination with a default collation");
    }
    TableDefinition definition{
        .table = destination,
        .schema = schema,
        .metadata = metadata,
        .keeps_rounding_modes = existing.has_value() || copy,
    };
    DatasetMetadata defaults = existing.has_value() ? DatasetMetadata{} : dataset;
    if (existing.has_value() && restore) {
      definition.metadata = existing->metadata;
      definition.metadata.clone.reset();
    } else if (existing.has_value()) {
      definition.metadata.default_rounding_mode = existing->metadata.default_rounding_mode;
    } else if (copy) {
      defaults.default_rounding_mode = RoundingMode::kUnspecified;
    }
    std::ranges::move(CreateTableWrite(definition, defaults).metadata_statements,
                      std::back_inserter(statements));
    std::ranges::move(ColumnDefaultStatements(destination, schema), std::back_inserter(statements));
    statements.push_back("INSERT INTO " + target + " SELECT * FROM " + result_table);
  } else {
    if (const int64_t creation_time = HistoryCreationTime(destination, existing->metadata);
        creation_time != 0) {
      kept = std::unique_lock(HistoryMutex(destination));
      std::ranges::move(KeepVersionStatements(destination, creation_time),
                        std::back_inserter(statements));
    }
    if (write == WriteDisposition::kWriteTruncateData) {
      statements.push_back("DELETE FROM " + target);
    }
    statements.push_back("INSERT INTO " + target + " BY NAME SELECT " + appended + " FROM " +
                         result_table);
    if (kept.owns_lock()) {
      statements.push_back(CommitVersionsStatement(destination.project_id));
    }
  }
  statements.emplace_back("COMMIT");
  statements.push_back("SELECT " + (count_only ? "count(*)" : ReadColumns(schema)) + " FROM " +
                       result_table);
  try {
    return backend_.ExecuteAll(statements, setup);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

}  // namespace bigquery_emulator_duckdb
