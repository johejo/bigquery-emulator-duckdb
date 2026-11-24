#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/change_data_capture.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/function_catalog/catalog.h"
#include "src/references.h"
#include "src/schema_sql.h"
#include "src/sql_runtime.h"
#include "src/table_metadata.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

namespace {

// The DuckDB expression for `value`, a value of `field` in a streamed row. DuckDB's
// casts round halfway values away from zero, so the values of ROUND_HALF_EVEN fields are read
// unrounded and rounded to even, as BigQuery rounds the rows it streams.
std::string RoundedInsertValue(const function_catalog::Catalog& functions, const json& value,
                               const FieldSchema& field, InsertValueOptions options) {
  const FieldSchema unrounded = WithoutRoundingParameters(field);
  const std::optional<std::string> rounded =
      TranslateAppendedValue(functions, InsertValue(value, unrounded, options), unrounded, field);
  if (!rounded.has_value()) {
    throw ApiError::Unsupported("streaming into ROUND_HALF_EVEN columns");
  }
  return *rounded;
}

// BigQuery checks lengths during protobuf decoding but not for Arrow. Neither staging
// nor publishing should check lengths again; keep the other destination constraints.
void ClearStorageWriteLengths(FieldSchema& field) {
  field.max_length.reset();
  for (auto& child : field.fields) {
    ClearStorageWriteLengths(child);
  }
}

// Convert a returned default from REST cells to the named input values used for staged
// writes. Keep decimal strings and bytes intact and format timestamps without a float roundtrip.
json StorageDefaultValue(const json& value, const FieldSchema& field) {
  if (value.is_null()) {
    return nullptr;
  }
  if (field.mode == FieldMode::kRepeated) {
    json result = json::array();
    for (const auto& cell : value) {
      result.push_back(StorageDefaultValue(cell.at("v"), AsNullable(field)));
    }
    return result;
  }
  if (field.type == FieldType::kRecord) {
    json result = json::object();
    for (size_t i = 0; i < field.fields.size(); ++i) {
      result[field.fields.at(i).name] =
          StorageDefaultValue(value.at("f").at(i).at("v"), field.fields.at(i));
    }
    return result;
  }
  if (field.type == FieldType::kRange) {
    const std::string text = value.get<std::string>();
    const size_t separator = text.find(", ");
    const auto bound = [&](const std::string& part) -> json {
      return part == "UNBOUNDED" ? json(nullptr)
                                 : StorageDefaultValue(part, RangeBoundField(field, field.name));
    };
    return json{
        {"start", bound(text.substr(1, separator - 1))},
        {"end", bound(text.substr(separator + 2, text.size() - separator - 3))},
    };
  }
  if (field.type == FieldType::kTimestamp) {
    return absl::FormatTime("%Y-%m-%d %H:%M:%E6S%Ez",
                            absl::FromUnixMicros(std::stoll(value.get<std::string>())),
                            absl::UTCTimeZone());
  }
  return value;
}

}  // namespace

TableDataPage SqlRuntime::ListTableData(TableReference table, int64_t start_index,
                                        int64_t max_results, std::string_view selected_fields) {
  auto read = BeginTableRead(std::move(table));
  const TableInfo& info = read.info;
  if (info.materialized) {
    throw ApiError::Invalid("Cannot list a table of type MATERIALIZED_VIEW.");
  }
  if (info.view_query) {
    throw ApiError::Invalid("Cannot read a view with tabledata.list; use a query instead");
  }
  const std::string columns = selected_fields.empty()
                                  ? ReadColumns(info.schema)
                                  : ReadColumns(SelectFields(info.schema, selected_fields), true);
  if (info.schema.empty()) {
    read.session->Transaction("COMMIT");
    return {};
  }
  try {
    const std::string relation = read.Relation();
    QueryResult result = read.session->Execute("SELECT " + columns + " FROM " + relation +
                                               " LIMIT " + std::to_string(max_results) +
                                               " OFFSET " + std::to_string(start_index));
    const auto count = read.session->Execute("SELECT count(*) FROM " + relation);
    const int64_t total_rows = count.rows.Int64(0, 0);
    read.session->Transaction("COMMIT");
    return {.result = std::move(result), .total_rows = total_rows};
  } catch (const BackendError& error) {
    throw ApiError::QueryFailure(error.what());
  }
}

std::vector<InsertError> SqlRuntime::WriteStorageRows(TableReference table,
                                                      const std::vector<FieldSchema>& schema,
                                                      json& rows, int64_t storage_id,
                                                      bool validate_only) {
  const auto expiration_lock = expiration_mutex_.LockCatalog();
  table.project_id = ResolveProject(table.project_id);
  const TableInfo info =
      GetTable(table, /*include_row_count=*/false, /*expire=*/true, &expiration_lock);
  table = info.reference;
  if (info.view_query) {
    throw ApiError::Invalid("Cannot write to a view: " + TableName(table));
  }
  RejectReadOnlyWrite(info);
  if (info.schema.empty()) {
    throw ApiError::Unsupported("writing rows to a table without a schema");
  }
  // Held until the transaction that keeps the table's rows and appends to it commits.
  std::unique_lock<absl::Mutex> kept;
  try {
    const auto session = backend_.NewSession();
    session->Transaction("BEGIN TRANSACTION");
    const QueryResult identity =
        session->Execute("SELECT table_oid FROM " +
                         RelationsQuery(table.project_id, table.dataset_id, table.table_id) +
                         " WHERE type = 'TABLE'");
    if (identity.rows.empty() || identity.rows.Int64(0, 0) != storage_id) {
      throw ApiError::NotFound("The Storage Write destination table was dropped or replaced");
    }
    CheckWritableTable(*session, table);
    if (SchemaToJson(TableSchema(*session, table)) != SchemaToJson(schema)) {
      throw ApiError::Invalid("Storage Write does not support destination schema changes");
    }
    if (!validate_only) {
      kept = std::unique_lock(HistoryMutex(table));
      (void)KeepVersion(*session, table);
    }
    std::vector<std::string> statements;
    constexpr InsertValueOptions options{.allow_interval_nanos = true};
    auto write_schema = schema;
    for (auto& field : write_schema) {
      ClearStorageWriteLengths(field);
    }
    for (size_t index = 0; index < rows.size(); ++index) {
      const auto& row = rows.at(index);
      std::vector<std::string> columns;
      std::vector<std::string> values;
      for (const auto& field : write_schema) {
        columns.push_back(QuoteIdentifier(field.name));
        const auto value = row.find(field.name);
        try {
          values.push_back(value == row.end()
                               ? "DEFAULT"
                               : RoundedInsertValue(functions_, *value, field, options));
        } catch (const ApiError& error) {
          return {{.index = index, .message = error.what()}};
        }
      }
      statements.push_back(std::format("INSERT INTO {} ({}) VALUES ({}){}", QualifiedName(table),
                                       Join(columns, ", "), Join(values, ", "),
                                       validate_only ? " RETURNING " + ReadColumns(schema) : ""));
    }
    for (size_t index = 0; index < statements.size(); ++index) {
      try {
        const QueryResult inserted = session->Execute(statements.at(index));
        if (validate_only) {
          auto& row = rows.at(index);
          const json returned = inserted.rows.at(0);
          for (size_t column = 0; column < schema.size(); ++column) {
            const auto& field = schema.at(column);
            if (!row.contains(field.name)) {
              row[field.name] = StorageDefaultValue(returned.at("f").at(column).at("v"), field);
            }
          }
        }
      } catch (const BackendError& error) {
        return {{.index = index, .message = error.what()}};
      }
    }
    session->Transaction(validate_only ? "ROLLBACK" : "COMMIT");
    return {};
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

std::vector<InsertError> SqlRuntime::WriteStorageChanges(TableReference table,
                                                         const std::vector<FieldSchema>& schema,
                                                         const json& rows, int64_t storage_id) {
  const auto expiration_lock = expiration_mutex_.LockCatalog();
  table.project_id = ResolveProject(table.project_id);
  const TableInfo info =
      GetTable(table, /*include_row_count=*/false, /*expire=*/true, &expiration_lock);
  table = info.reference;
  if (info.view_query) {
    throw ApiError::Invalid("Cannot write to a view: " + TableName(table));
  }
  RejectReadOnlyWrite(info);
  const std::vector<std::string> key = PrimaryKeyColumns(info.metadata.primary_key, info.schema);
  if (key.empty()) {
    throw ApiError::Invalid("Change data capture requires a table with a primary key: " +
                            TableName(table));
  }
  if (key.size() > 16) {
    throw ApiError::Unsupported("change data capture on primary keys of more than 16 columns");
  }
  std::vector<const FieldSchema*> key_fields;
  for (const std::string& name : key) {
    const auto field = std::ranges::find(schema, name, &FieldSchema::name);
    if (field == schema.end()) {
      throw ApiError::Invalid("Storage Write does not support destination schema changes");
    }
    if (field->collation.has_value() && !field->collation->empty()) {
      throw ApiError::Unsupported("change data capture on collated primary key columns");
    }
    key_fields.push_back(&*field);
  }
  const int64_t creation_time = HistoryCreationTime(table, info.metadata);
  auto write_schema = schema;
  for (auto& field : write_schema) {
    ClearStorageWriteLengths(field);
  }
  constexpr InsertValueOptions options{.allow_interval_nanos = true};

  // What each row asks for, and the SQL of its key's values.
  struct Change {
    bool deletes = false;
    std::optional<ChangeSequence> sequence;
    std::vector<std::string> key_values;
    std::string key;
  };
  std::vector<Change> changes(rows.size());
  for (size_t index = 0; index < rows.size(); ++index) {
    const json& row = rows.at(index);
    Change& change = changes.at(index);
    const auto type = row.find(kChangeTypeColumn);
    const std::string type_text =
        type != row.end() && type->is_string() ? type->get<std::string>() : "";
    if (type_text == "UPSERT" || type_text == "DELETE") {
      change.deletes = type_text == "DELETE";
    } else if (type == row.end() || type->is_null() ||
               absl::EqualsIgnoreCase(type_text, "UPSERT") ||
               absl::EqualsIgnoreCase(type_text, "DELETE") ||
               absl::EqualsIgnoreCase(type_text, "INSERT")) {
      // Whether BigQuery takes INSERT, a missing value or other cases is not known.
      throw ApiError::Unsupported(std::string(kChangeTypeColumn) + " values other than " +
                                  "UPSERT and DELETE");
    } else {
      return {{.index = index, .message = "The given value is not a valid CHANGE_TYPE"}};
    }
    const auto sequence = row.find(kChangeSequenceColumn);
    if (sequence != row.end() && !sequence->is_null()) {
      const std::string text = sequence->get<std::string>();
      if (text.empty()) {
        throw ApiError::Unsupported("empty " + std::string(kChangeSequenceColumn) + " values");
      }
      if (creation_time == 0) {
        throw ApiError::Unsupported(std::string(kChangeSequenceColumn) +
                                    " on tables created before the emulator recorded creation "
                                    "times");
      }
      change.sequence = ParseChangeSequence(text);
      if (!change.sequence) {
        return {
            {
                .index = index,
                .message = "Invalid " + std::string(kChangeSequenceColumn) + ": " + text,
            },
        };
      }
    }
    for (const FieldSchema* field : key_fields) {
      const auto value = row.find(field->name);
      if (value == row.end() || value->is_null()) {
        throw ApiError::Unsupported(
            "change data capture rows without a value for primary key "
            "column " +
            field->name);
      }
      try {
        change.key_values.push_back(RoundedInsertValue(functions_, *value, *field, options));
      } catch (const ApiError& error) {
        return {{.index = index, .message = error.what()}};
      }
    }
  }
  std::unique_lock<absl::Mutex> kept;
  try {
    const auto session = backend_.NewSession();
    session->Transaction("BEGIN TRANSACTION");
    const QueryResult identity =
        session->Execute("SELECT table_oid FROM " +
                         RelationsQuery(table.project_id, table.dataset_id, table.table_id) +
                         " WHERE type = 'TABLE'");
    if (identity.rows.empty() || identity.rows.Int64(0, 0) != storage_id) {
      throw ApiError::NotFound("The Storage Write destination table was dropped or replaced");
    }
    CheckWritableTable(*session, table);
    if (SchemaToJson(TableSchema(*session, table)) != SchemaToJson(schema)) {
      throw ApiError::Invalid("Storage Write does not support destination schema changes");
    }
    kept = std::unique_lock(HistoryMutex(table));
    (void)KeepVersion(*session, table);
    if (rows.empty()) {
      session->Transaction("COMMIT");
      return {};
    }

    // DuckDB spells each key canonically, as the change sequence table records it.
    std::vector<std::string> selects;
    selects.reserve(changes.size());
    for (size_t index = 0; index < changes.size(); ++index) {
      selects.push_back(std::format("SELECT {} AS i, CAST(json_array({}) AS VARCHAR) AS key", index,
                                    Join(changes.at(index).key_values, ", ")));
    }
    const std::string keys_query = Join(selects, " UNION ALL ");
    const auto keys = session->Execute("SELECT key FROM (" + keys_query + ") ORDER BY i");
    const auto key_texts = keys.TextRows();
    for (size_t index = 0; index < changes.size(); ++index) {
      changes.at(index).key = key_texts.at(index).at(0).value_or("");
    }

    // The sequence number in effect for each key, and whether it deleted the key.
    struct Recorded {
      ChangeSequence sequence;
      bool deleted = false;
    };
    std::unordered_map<std::string, Recorded> recorded;
    const int64_t now = absl::ToUnixMicros(absl::Now());
    const auto stored = session->Execute(std::format(
        "SELECT key, sequence, CAST(deleted AS VARCHAR) FROM {} WHERE {} AND (NOT deleted OR "
        "changed_micros >= {}) AND key IN (SELECT key FROM ({}))",
        ChangeSequenceTable(table.project_id), ChangeSequencesOf(table, creation_time),
        now - kDeleteRetentionMicros, keys_query));
    for (const auto& row : stored.TextRows()) {
      const auto sequence = ChangeSequence::FromText(row.at(1).value_or(""));
      if (!sequence) {
        throw ApiError::Internal("Invalid recorded change sequence: " + row.at(1).value_or(""));
      }
      recorded[*row.at(0)] = {.sequence = *sequence, .deleted = row.at(2) == "true"};
    }

    // The row whose change takes effect for each key, in the order the keys first appear.
    std::unordered_map<std::string, size_t> effective;
    std::vector<std::string> order;
    std::set<std::string> sequenced;
    for (size_t index = 0; index < changes.size(); ++index) {
      const Change& change = changes.at(index);
      const auto previous = recorded.find(change.key);
      if (change.sequence && previous != recorded.end()) {
        const auto compared = CompareChangeSequences(*change.sequence, previous->second.sequence);
        if (!compared) {
          throw ApiError::Unsupported("comparing " + std::string(kChangeSequenceColumn) +
                                      " values that differ only in trailing zero sections");
        }
        // Of equal sequence numbers, the change ingested last takes effect.
        if (*compared == std::strong_ordering::less) {
          continue;
        }
      }
      if (change.sequence) {
        recorded[change.key] = {.sequence = *change.sequence, .deleted = change.deletes};
        sequenced.insert(change.key);
      }
      if (effective.emplace(change.key, index).second) {
        order.push_back(change.key);
      } else {
        effective[change.key] = index;
      }
    }

    std::vector<std::string> columns;
    std::ranges::transform(write_schema, std::back_inserter(columns),
                           [](const FieldSchema& field) { return QuoteIdentifier(field.name); });
    for (const std::string& changed : order) {
      const size_t index = effective.at(changed);
      const Change& change = changes.at(index);
      std::vector<std::string> conditions;
      conditions.reserve(key_fields.size());
      for (size_t column = 0; column < key_fields.size(); ++column) {
        conditions.push_back(QuoteIdentifier(key_fields.at(column)->name) + " = " +
                             change.key_values.at(column));
      }
      // BigQuery does not enforce the key, and what a change does to a key that several rows
      // share is not known.
      const QueryResult removed = session->Execute(
          std::format("DELETE FROM {} WHERE {}", QualifiedName(table), Join(conditions, " AND ")));
      if (removed.affected_rows > 1) {
        throw ApiError::Unsupported(
            "change data capture rows whose primary key several rows share");
      }
      if (change.deletes) {
        continue;
      }
      const json& row = rows.at(index);
      try {
        std::vector<std::string> values;
        for (const auto& field : write_schema) {
          const auto value = row.find(field.name);
          values.push_back(value == row.end()
                               ? "DEFAULT"
                               : RoundedInsertValue(functions_, *value, field, options));
        }
        session->Execute(std::format("INSERT INTO {} ({}) VALUES ({})", QualifiedName(table),
                                     Join(columns, ", "), Join(values, ", ")));
      } catch (const ApiError& error) {
        return {{.index = index, .message = error.what()}};
      } catch (const BackendError& error) {
        return {{.index = index, .message = error.what()}};
      }
    }
    for (const std::string& changed : sequenced) {
      const Recorded& state = recorded.at(changed);
      session->Execute(
          std::format("INSERT OR REPLACE INTO {} VALUES ({}, {}, {}, {}, {}, {}, {})",
                      ChangeSequenceTable(table.project_id), QuoteLiteral(table.dataset_id),
                      QuoteLiteral(table.table_id), creation_time, QuoteLiteral(changed),
                      QuoteLiteral(state.sequence.Text()), state.deleted ? "true" : "false", now));
    }
    session->Transaction("COMMIT");
    return {};
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

std::vector<InsertError> SqlRuntime::InsertTableData(TableReference table, const json& rows,
                                                     bool skip_invalid_rows,
                                                     bool ignore_unknown_values) {
  const auto expiration_lock = expiration_mutex_.LockCatalog();
  table.project_id = ResolveProject(table.project_id);
  if (!rows.is_array()) {
    throw ApiError::Invalid("rows must be an array");
  }
  const TableInfo info =
      GetTable(table, /*include_row_count=*/false, /*expire=*/true, &expiration_lock);
  table = info.reference;
  if (info.view_query) {
    throw ApiError::Invalid("Cannot insert into a view: " + TableName(table));
  }
  if (info.metadata.snapshot.has_value()) {
    throw ApiError::Invalid("Cannot add rows to a table of type SNAPSHOT.");
  }
  CheckWritableTable(backend_, table);
  if (info.schema.empty()) {
    throw ApiError::Invalid("The destination table has no schema.");
  }
  const std::vector<FieldSchema>& schema = info.schema;
  std::vector<InsertError> errors;
  std::vector<std::string> statements;
  std::vector<size_t> indexes;
  const InsertValueOptions options{.ignore_unknown_values = ignore_unknown_values};
  for (size_t i = 0; i < rows.size(); ++i) {
    try {
      if (!rows[i].is_object() || !rows[i].contains("json") || !rows[i]["json"].is_object()) {
        throw ApiError::Invalid("Row must contain a json object");
      }
      const json& values = rows[i]["json"];
      for (auto it = values.begin(); it != values.end(); ++it) {
        const bool known = std::ranges::any_of(
            schema, [&](const FieldSchema& field) { return field.name == it.key(); });
        if (!known && !ignore_unknown_values) {
          throw ApiError::Invalid("Unknown field: " + it.key());
        }
      }
      std::vector<std::string> columns;
      std::vector<std::string> literals;
      for (const FieldSchema& field : schema) {
        columns.push_back(QuoteIdentifier(field.name));
        const auto it = values.find(field.name);
        literals.push_back(
            it == values.end() ? "DEFAULT" : RoundedInsertValue(functions_, *it, field, options));
      }
      std::string statement = "INSERT INTO ";
      statement += QualifiedName(table);
      statement += " (";
      statement += Join(columns, ", ");
      statement += ") VALUES (";
      statement += Join(literals, ", ");
      statement += ')';
      statements.push_back(std::move(statement));
      indexes.push_back(i);
    } catch (const ApiError& error) {
      errors.push_back({.index = i, .message = error.what()});
    }
  }
  if (!skip_invalid_rows && !errors.empty()) {
    return errors;
  }
  try {
    const std::unique_lock<absl::Mutex> kept =
        statements.empty() ? std::unique_lock<absl::Mutex>() : KeepVersion(backend_, table);
    for (const auto& [index, message] : backend_.InsertRows(statements, skip_invalid_rows)) {
      errors.push_back({.index = indexes.at(index), .message = message});
    }
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
  std::ranges::sort(errors,
                    [](const InsertError& a, const InsertError& b) { return a.index < b.index; });
  return errors;
}

}  // namespace bigquery_emulator_duckdb
