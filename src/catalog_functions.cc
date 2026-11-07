#include "src/catalog_functions.h"

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "duckdb.h"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "src/backend_error.h"

namespace bigquery_emulator_duckdb {

namespace {

using duckdb::CatalogEntry;
using duckdb::CatalogType;
using duckdb::ClientContext;
using duckdb::DataChunk;
using duckdb::FunctionData;
using duckdb::GlobalTableFunctionState;
using duckdb::Identifier;
using duckdb::LogicalType;
using duckdb::SchemaCatalogEntry;
using duckdb::TableFunction;
using duckdb::TableFunctionBindInput;
using duckdb::TableFunctionInitInput;
using duckdb::TableFunctionInput;
using duckdb::Value;

// The stored names a call selects; each one that is set must match exactly. DuckDB's own lookups
// ignore case, while the emulator stores names that differ only in case under distinct encodings.
struct RelationFilter : FunctionData {
  std::optional<std::string> database;
  std::optional<std::string> schema;
  std::optional<std::string> name;

  [[nodiscard]] duckdb::unique_ptr<FunctionData> Copy() const override {
    return duckdb::make_uniq<RelationFilter>(*this);
  }
  [[nodiscard]] bool Equals(const FunctionData& other) const override {
    const auto& filter = other.Cast<RelationFilter>();
    return database == filter.database && schema == filter.schema && name == filter.name;
  }
};

std::optional<std::string> NamedString(TableFunctionBindInput& input, const char* name) {
  const auto it = input.named_parameters.find(name);
  if (it == input.named_parameters.end() || it->second.IsNull()) {
    return std::nullopt;
  }
  return it->second.ToString();
}

bool Matches(const std::optional<std::string>& wanted, const Identifier& name) {
  return !wanted.has_value() || *wanted == name.GetIdentifierName();
}

// The tables and views `filter` selects, found by lookup when it names them.
std::vector<std::reference_wrapper<CatalogEntry>> FindRelations(ClientContext& context,
                                                                const RelationFilter& filter) {
  std::vector<std::reference_wrapper<CatalogEntry>> entries;
  const auto add = [&](CatalogEntry& entry) {
    if ((entry.type == CatalogType::TABLE_ENTRY || entry.type == CatalogType::VIEW_ENTRY) &&
        Matches(filter.name, entry.name)) {
      entries.emplace_back(entry);
    }
  };
  if (!filter.database.has_value()) {
    for (SchemaCatalogEntry& schema : duckdb::Catalog::GetAllSchemas(context)) {
      if (Matches(filter.schema, schema.name)) {
        schema.Scan(context, CatalogType::TABLE_ENTRY, add);
      }
    }
    return entries;
  }
  auto catalog = duckdb::Catalog::GetCatalogEntry(context, Identifier(*filter.database));
  if (!catalog || catalog->GetName().GetIdentifierName() != *filter.database) {
    return entries;
  }
  if (!filter.schema.has_value()) {
    catalog->ScanSchemas(context, [&](SchemaCatalogEntry& schema) {
      schema.Scan(context, CatalogType::TABLE_ENTRY, add);
    });
    return entries;
  }
  auto schema =
      catalog->GetSchema(context,
                         duckdb::EntryLookupInfo(CatalogType::SCHEMA_ENTRY,
                                                 duckdb::QualifiedName(Identifier(*filter.schema))),
                         duckdb::OnEntryNotFound::RETURN_NULL);
  if (!schema || schema->name.GetIdentifierName() != *filter.schema) {
    return entries;
  }
  if (!filter.name.has_value()) {
    schema->Scan(context, CatalogType::TABLE_ENTRY, add);
    return entries;
  }
  // A table lookup also finds a view, since they share a namespace.
  if (auto entry = schema->GetEntry(catalog->GetCatalogTransaction(context),
                                    CatalogType::TABLE_ENTRY, Identifier(*filter.name))) {
    add(*entry);
  }
  return entries;
}

struct EntriesState : GlobalTableFunctionState {
  std::vector<std::reference_wrapper<CatalogEntry>> entries;
  size_t offset = 0;
};

duckdb::unique_ptr<FunctionData> RelationsBind(ClientContext& /*context*/,
                                               TableFunctionBindInput& input,
                                               duckdb::vector<LogicalType>& types,
                                               duckdb::vector<Identifier>& names) {
  for (const char* name : {"database_name", "schema_name", "table_name", "type", "comment"}) {
    names.emplace_back(name);
    types.emplace_back(LogicalType::VARCHAR);
  }
  names.emplace_back("table_oid");
  types.emplace_back(LogicalType::BIGINT);
  auto filter = duckdb::make_uniq<RelationFilter>();
  filter->database = NamedString(input, "database");
  filter->schema = NamedString(input, "schema");
  filter->name = NamedString(input, "name");
  return filter;
}

duckdb::unique_ptr<GlobalTableFunctionState> RelationsInit(ClientContext& context,
                                                           TableFunctionInitInput& input) {
  auto state = duckdb::make_uniq<EntriesState>();
  state->entries = FindRelations(context, input.bind_data->Cast<RelationFilter>());
  return state;
}

void RelationsScan(ClientContext& /*context*/, TableFunctionInput& input, DataChunk& output) {
  auto& state = input.global_state->Cast<EntriesState>();
  duckdb::idx_t count = 0;
  while (state.offset < state.entries.size() && count < STANDARD_VECTOR_SIZE) {
    const CatalogEntry& entry = state.entries[state.offset++];
    const auto& standard = entry.Cast<duckdb::StandardEntry>();
    output.data[0].Append(Value(standard.ParentCatalog().GetName()));
    output.data[1].Append(Value(standard.schema.name));
    output.data[2].Append(Value(entry.name));
    output.data[3].Append(Value(entry.type == CatalogType::VIEW_ENTRY ? "VIEW" : "TABLE"));
    output.data[4].Append(entry.comment.IsNull() ? Value() : Value(entry.comment.ToString()));
    output.data[5].Append(Value::BIGINT(static_cast<int64_t>(entry.oid)));
    ++count;
  }
}

struct ColumnCommentsState : GlobalTableFunctionState {
  std::vector<Value> comments;
  size_t offset = 0;
};

duckdb::unique_ptr<FunctionData> ColumnCommentsBind(ClientContext& /*context*/,
                                                    TableFunctionBindInput& input,
                                                    duckdb::vector<LogicalType>& types,
                                                    duckdb::vector<Identifier>& names) {
  names.emplace_back("column_index");
  types.emplace_back(LogicalType::BIGINT);
  names.emplace_back("comment");
  types.emplace_back(LogicalType::VARCHAR);
  auto filter = duckdb::make_uniq<RelationFilter>();
  for (size_t i = 0; i < 3; ++i) {
    if (input.inputs.at(i).IsNull()) {
      throw duckdb::InvalidInputException(
          "bq_column_comments requires a database, schema and name");
    }
  }
  filter->database = input.inputs.at(0).ToString();
  filter->schema = input.inputs.at(1).ToString();
  filter->name = input.inputs.at(2).ToString();
  return filter;
}

duckdb::unique_ptr<GlobalTableFunctionState> ColumnCommentsInit(ClientContext& context,
                                                                TableFunctionInitInput& input) {
  auto state = duckdb::make_uniq<ColumnCommentsState>();
  for (CatalogEntry& entry : FindRelations(context, input.bind_data->Cast<RelationFilter>())) {
    if (entry.type == CatalogType::TABLE_ENTRY) {
      for (const auto& column : entry.Cast<duckdb::TableCatalogEntry>().GetColumns().Logical()) {
        state->comments.push_back(column.Comment());
      }
      continue;
    }
    // As duckdb_columns() does, a view whose query no longer binds has no column comments.
    auto& view = entry.Cast<duckdb::ViewCatalogEntry>();
    try {
      view.BindView(context);
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): reported as unbound.
    }
    if (const auto columns = view.GetColumnInfo()) {
      for (size_t i = 0; i < columns->names.size(); ++i) {
        state->comments.push_back(view.GetColumnComment(i));
      }
    }
  }
  return state;
}

void ColumnCommentsScan(ClientContext& /*context*/, TableFunctionInput& input, DataChunk& output) {
  auto& state = input.global_state->Cast<ColumnCommentsState>();
  duckdb::idx_t count = 0;
  while (state.offset < state.comments.size() && count < STANDARD_VECTOR_SIZE) {
    const Value& comment = state.comments[state.offset];
    output.data[0].Append(Value::BIGINT(static_cast<int64_t>(++state.offset)));
    output.data[1].Append(comment.IsNull() ? Value() : Value(comment.ToString()));
    ++count;
  }
}

}  // namespace

void RegisterCatalogFunctions(duckdb_database database) {
  try {
    duckdb::DatabaseInstance& instance =
        *reinterpret_cast<duckdb::DatabaseWrapper*>(database)->database->instance;
    duckdb::ExtensionLoader loader(instance, "bigquery_emulator_catalog");
    loader.RegisterFunction(TableFunction("bq_relations",
                                          duckdb::FunctionSignature().AddTypedKwargs(
                                              "filter", duckdb::TypedKwargs()
                                                            .Add("database", LogicalType::VARCHAR)
                                                            .Add("schema", LogicalType::VARCHAR)
                                                            .Add("name", LogicalType::VARCHAR)),
                                          RelationsScan, RelationsBind, RelationsInit));
    loader.RegisterFunction(TableFunction(
        "bq_column_comments", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
        ColumnCommentsScan, ColumnCommentsBind, ColumnCommentsInit));
  } catch (const std::exception& error) {
    throw BackendError(error.what());
  }
}

}  // namespace bigquery_emulator_duckdb
