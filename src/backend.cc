#include "src/backend.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "duckdb.h"
#include "duckdb_static_extension.h"
#include "nlohmann/json.hpp"
#include "src/backend_error.h"
#include "src/backend_functions.h"
#include "src/backend_functions/javascript_limits.h"
#include "src/backend_functions/object_metadata.h"
#include "src/duckdb_handle.h"
#include "src/duckdb_sql.h"
#include "src/duckpgq.h"
#include "src/field_schema.h"
#include "src/mysql_query.h"
#include "src/postgres_scanner.h"
#include "src/result_chunks.h"
#include "src/result_rows.h"
#include "src/storage_snapshot.h"
#include "src/type_mapping.h"

// DuckDB v2 requires explicit registration of statically linked extensions before opening a
// database. Keep using duckdb.h's V1 C API for database access.
extern "C" {
int32_t duckdb_extension_autocomplete_describe(duckdb_extension_descriptor*);
int32_t duckdb_extension_core_functions_describe(duckdb_extension_descriptor*);
int32_t duckdb_extension_icu_describe(duckdb_extension_descriptor*);
int32_t duckdb_extension_json_describe(duckdb_extension_descriptor*);
int32_t duckdb_extension_parquet_describe(duckdb_extension_descriptor*);
}

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

struct Result {
  duckdb_result result{};
  ~Result() { duckdb_destroy_result(&result); }
  Result() = default;
  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;
};

// Statements that produce a result set. Everything else (DDL, DML, SET, ...) yields a
// single "Count" or "Success" column that is not part of the BigQuery result.
bool ProducesResultSet(duckdb_statement_type type) {
  return type == DUCKDB_STATEMENT_TYPE_SELECT || type == DUCKDB_STATEMENT_TYPE_EXPLAIN ||
         type == DUCKDB_STATEMENT_TYPE_PRAGMA || type == DUCKDB_STATEMENT_TYPE_CALL ||
         type == DUCKDB_STATEMENT_TYPE_EXECUTE;
}

bool IsDml(duckdb_statement_type type) {
  return type == DUCKDB_STATEMENT_TYPE_INSERT || type == DUCKDB_STATEMENT_TYPE_UPDATE ||
         type == DUCKDB_STATEMENT_TYPE_DELETE || type == DUCKDB_STATEMENT_TYPE_MERGE_INTO;
}

// DuckDB's message for a failed statement. DuckDB's error() raises the messages the translation
// writes as BigQuery's, prefixed with the class of exception it raises, which is left out.
std::string ErrorMessage(const char* error, std::string_view fallback) {
  constexpr std::string_view kInvalidInput = "Invalid Input Error: ";
  if (error == nullptr) {
    return std::string(fallback);
  }
  std::string_view message = error;
  message.remove_prefix(message.starts_with(kInvalidInput) ? kInvalidInput.size() : 0);
  return std::string(message);
}

void Query(duckdb_connection connection, const std::string& sql, Result& result) {
  if (duckdb_query(connection, sql.c_str(), &result.result) == DuckDBError) {
    throw BackendError(ErrorMessage(duckdb_result_error(&result.result), "DuckDB query failed"));
  }
}

void Query(duckdb_connection connection, const std::string& sql) {
  Result result;
  Query(connection, sql, result);
}

QueryResult Materialize(Result& result, bool null_arrays, std::shared_ptr<const void> owner) {
  const auto statement_type = duckdb_result_statement_type(result.result);
  QueryResult query_result;
  if (!ProducesResultSet(statement_type)) {
    if (IsDml(statement_type)) {
      query_result.affected_rows = static_cast<int64_t>(duckdb_rows_changed(&result.result));
    }
    return query_result;
  }
  query_result.has_rows = true;
  for (idx_t i = 0; i < duckdb_column_count(&result.result); ++i) {
    LogicalType const type(duckdb_column_logical_type(&result.result, i));
    query_result.schema.push_back(
        ResultFieldSchema(duckdb_column_name(&result.result, i), type.get()));
  }
  query_result.rows = ResultRows(
      std::make_shared<const ResultChunks>(result.result, std::move(owner)), null_arrays);
  return query_result;
}

// Prepares `sql`, which binds names and types without running the statement.
Prepared PrepareStatement(duckdb_connection connection, const std::string& sql) {
  Prepared prepared;
  if (duckdb_prepare(connection, sql.c_str(), prepared.out()) == DuckDBError) {
    throw BackendError(
        ErrorMessage(duckdb_prepare_error(prepared.get()), "DuckDB failed to prepare the query"));
  }
  return prepared;
}

struct Column {
  std::string name;
  LogicalType type;
};

// The result columns of a prepared statement.
std::vector<Column> Columns(const Prepared& prepared) {
  std::vector<Column> columns;
  for (idx_t i = 0; i < duckdb_prepared_statement_column_count(prepared.get()); ++i) {
    DuckString const name(duckdb_prepared_statement_column_name(prepared.get(), i));
    columns.push_back({
        .name = name.get(),
        .type = LogicalType(duckdb_prepared_statement_column_logical_type(prepared.get(), i)),
    });
  }
  return columns;
}

// BigQuery evaluates timestamps in UTC; setup must run on the same connection.
void RunSetup(duckdb_connection connection, const std::vector<std::string>& setup) {
  Query(connection, "SET TimeZone = 'UTC'");
  for (const std::string& statement : setup) {
    Query(connection, statement);
  }
}

}  // namespace

json QueryResult::SchemaToJson() const {
  std::vector<FieldSchema> given = schema;
  SetModesGiven(given);
  return bigquery_emulator_duckdb::SchemaToJson(given);
}

struct Backend::Database {
  Handle<duckdb_database, duckdb_close> handle;
  Database(const std::optional<std::string>& session_user,
           const backend_functions::JavaScriptLimits& javascript,
           const backend_functions::ObjectStorage& storage) {
    if (session_user && session_user->empty()) {
      throw std::invalid_argument("session_user must not be empty");
    }
    static const bool registered = [] {
      for (const auto describe : {
               duckdb_extension_autocomplete_describe,
               duckdb_extension_core_functions_describe,
               duckdb_extension_icu_describe,
               duckdb_extension_json_describe,
               duckdb_extension_parquet_describe,
           }) {
        if (duckdb_register_static_extension(describe) != 0) {
          throw BackendError("DuckDB failed to register a static extension");
        }
      }
      return true;
    }();
    (void)registered;
    Handle<duckdb_config, duckdb_destroy_config> config;
    // Catalog discovery can encounter missing objects while a script transaction is open.
    // Preserve the pre-v2 policy: binding errors must not abort that transaction.
    if (duckdb_create_config(config.out()) == DuckDBError ||
        duckdb_set_config(config.get(), "default_transaction_invalidation_policy",
                          "SYNTACTIC_ERRORS_DO_NOT_INVALIDATE") == DuckDBError) {
      throw BackendError("DuckDB failed to configure transaction invalidation");
    }
    char* error = nullptr;
    const duckdb_state state = duckdb_open_ext(nullptr, handle.out(), config.get(), &error);
    DuckString const message(error);
    if (state == DuckDBError) {
      throw BackendError(message ? message.get() : "DuckDB failed to open database");
    }
    RegisterBackendFunctions(handle.get(), session_user, javascript, storage);
    RegisterPostgresScanner(handle.get());
    RegisterMySqlScanner(handle.get());
    RegisterDuckPGQ(handle.get());
  }

  [[nodiscard]] Connection Connect() const {
    Connection connection;
    if (duckdb_connect(handle.get(), connection.out()) == DuckDBError) {
      throw BackendError("DuckDB failed to connect");
    }
    Query(connection.get(), "SET active_grammar_extensions = ['duckpgq']");
    Query(connection.get(), "SET duckpgq_vertex_join_elimination = false");
    Query(connection.get(), "SET duckpgq_factorized_joins = false");
    return Connection(connection.release());
  }
};

struct Backend::Session {
  Connection connection;
  bool in_transaction = false;
  std::vector<std::string> before_commit;
  explicit Session(Database& database) : connection(database.Connect()) {}
};

class Backend::CallConnection {
 public:
  explicit CallConnection(duckdb_connection session) : connection_(session) {}
  explicit CallConnection(Connection owned) : owned_(std::move(owned)), connection_(owned_.get()) {}

  [[nodiscard]] duckdb_connection get() const { return connection_; }

 private:
  Connection owned_;
  duckdb_connection connection_;
};

Backend::CallConnection Backend::Connect() const {
  return session_ ? CallConnection(session_->connection.get()) : CallConnection(db_->Connect());
}

Backend::Backend(const std::optional<std::string>& session_user,
                 const backend_functions::JavaScriptLimits& javascript,
                 const backend_functions::ObjectStorage& storage)
    : db_(std::make_shared<Database>(session_user, javascript, storage)) {}

Backend::Backend(std::shared_ptr<Database> database)
    : db_(std::move(database)), session_(std::make_unique<Session>(*db_)) {}

std::unique_ptr<Backend> Backend::NewSession() {
  return std::unique_ptr<Backend>(new Backend(db_));
}

Backend::~Backend() = default;

void Backend::Transaction(const std::string& statement) {
  if (!session_) {
    throw BackendError("Transactions require a session");
  }
  const std::vector<std::string> before_commit = std::exchange(session_->before_commit, {});
  if (statement == "COMMIT") {
    for (const std::string& pending : before_commit) {
      Query(session_->connection.get(), pending);
    }
  }
  Query(session_->connection.get(), statement);
  session_->in_transaction = statement == "BEGIN TRANSACTION";
}

bool Backend::InTransaction() const { return session_ && session_->in_transaction; }

void Backend::BeforeCommit(const std::string& statement) {
  if (!InTransaction()) {
    throw BackendError("No transaction to commit");
  }
  if (std::ranges::find(session_->before_commit, statement) == session_->before_commit.end()) {
    session_->before_commit.push_back(statement);
  }
}

QueryResult Backend::Execute(const std::string& sql, const std::vector<std::string>& setup,
                             bool null_arrays) {
  return ExecuteAll({sql}, setup, null_arrays);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): setup runs before the statements.
QueryResult Backend::ExecuteAll(const std::vector<std::string>& statements,
                                const std::vector<std::string>& setup, bool null_arrays) {
  if (statements.empty()) {
    throw BackendError("No statement to execute");
  }
  const CallConnection connection = Connect();
  RunSetup(connection.get(), setup);
  // A transaction left open by a failed statement is rolled back when the connection closes.
  for (size_t i = 0; i + 1 < statements.size(); ++i) {
    Query(connection.get(), statements.at(i));
  }
  Result result;
  Query(connection.get(), statements.back(), result);
  return Materialize(result, null_arrays, db_);
}

std::shared_ptr<const StorageSnapshot> Backend::ExecuteStorageRead(
    const std::string& sql, std::vector<FieldSchema> schema) {
  const auto session = std::shared_ptr<Backend>(NewSession());
  const CallConnection connection = session->Connect();
  RunSetup(connection.get(), {});
  Result result;
  Query(connection.get(), sql, result);
  return std::make_shared<const StorageSnapshot>(result.result, std::move(schema), session);
}

void Backend::ExecuteDdl(const std::string& sql,
                         const std::vector<std::string>& metadata_statements,
                         const std::string& skip_query, const std::vector<std::string>& setup) {
  const CallConnection connection = Connect();
  RunSetup(connection.get(), setup);
  const bool own_transaction = !session_ || !session_->in_transaction;
  if (own_transaction) {
    Query(connection.get(), "BEGIN TRANSACTION");
  }
  try {
    if (!skip_query.empty()) {
      Result existing;
      Query(connection.get(), skip_query, existing);
      if (duckdb_row_count(&existing.result) != 0) {
        if (own_transaction) {
          Query(connection.get(), "COMMIT");
        }
        return;
      }
    }
    Query(connection.get(), sql);
    for (const std::string& statement : metadata_statements) {
      Query(connection.get(), statement);
    }
    if (own_transaction) {
      Query(connection.get(), "COMMIT");
    }
  } catch (...) {
    if (own_transaction && session_) {
      Query(connection.get(), "ROLLBACK");
    }
    throw;
  }
}

QueryResult Backend::Prepare(const std::string& sql, const std::vector<std::string>& setup) {
  const CallConnection connection = Connect();
  RunSetup(connection.get(), setup);
  // A dry run is validated and knows its result schema, but reads and writes nothing.
  const Prepared prepared = PrepareStatement(connection.get(), sql);
  QueryResult query_result;
  if (!ProducesResultSet(duckdb_prepared_statement_type(prepared.get()))) {
    return query_result;
  }
  query_result.has_rows = true;
  for (const Column& column : Columns(prepared)) {
    query_result.schema.push_back(ResultFieldSchema(column.name, column.type.get()));
  }
  return query_result;
}

std::optional<std::string> Backend::UnreadableColumnError(const std::string& relation) {
  const CallConnection connection = Connect();
  for (const Column& column :
       Columns(PrepareStatement(connection.get(), "SELECT * FROM " + relation))) {
    if (const std::string type = UnsupportedDuckDbType(column.type.get()); !type.empty()) {
      return "Column " + column.name + " has the DuckDB type " + type +
             ", which the emulator does not support";
    }
  }
  return std::nullopt;
}

std::optional<std::string> Backend::NumericReadQuery(const std::string& relation) {
  const CallConnection connection = Connect();
  std::vector<std::string> columns;
  bool changed = false;
  for (const Column& column :
       Columns(PrepareStatement(connection.get(), "SELECT * FROM " + relation))) {
    const std::string name = QuoteIdentifier(column.name);
    const auto converted = ReadNumericStorage(name, column.type.get());
    changed |= converted.has_value();
    columns.push_back(converted.value_or(name) + " AS " + name);
  }
  return changed ? std::make_optional("SELECT " + Join(columns, ", ") + " FROM " + relation)
                 : std::nullopt;
}

std::vector<std::pair<size_t, std::string>> Backend::InsertRows(
    const std::vector<std::string>& statements, bool skip_invalid_rows) {
  const CallConnection connection = Connect();
  RunSetup(connection.get(), {});
  std::vector<std::pair<size_t, std::string>> errors;
  if (!skip_invalid_rows) {
    Query(connection.get(), "BEGIN TRANSACTION");
  }
  for (size_t i = 0; i < statements.size(); ++i) {
    Result result;
    if (duckdb_query(connection.get(), statements.at(i).c_str(), &result.result) == DuckDBError) {
      const char* error = duckdb_result_error(&result.result);
      errors.emplace_back(i, error != nullptr ? error : "DuckDB insert failed");
      if (!skip_invalid_rows) {
        Query(connection.get(), "ROLLBACK");
        return errors;
      }
    }
  }
  if (!skip_invalid_rows) {
    Query(connection.get(), "COMMIT");
  }
  return errors;
}

}  // namespace bigquery_emulator_duckdb
