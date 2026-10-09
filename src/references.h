#pragma once

#include <string>

#include "src/duckdb_sql.h"

namespace bigquery_emulator_duckdb {

struct DatasetReference {
  std::string project_id;
  std::string dataset_id;
};

struct TableReference {
  std::string project_id;
  std::string dataset_id;
  std::string table_id;
};

struct RoutineReference {
  std::string project_id;
  std::string dataset_id;
  std::string routine_id;
};

// The DuckDB name of a dataset, which is a schema in the project's catalog.
inline std::string QualifiedName(const DatasetReference& dataset) {
  return QuoteIdentifier(dataset.project_id) + "." + QuoteIdentifier(dataset.dataset_id);
}

inline std::string QualifiedName(const TableReference& table) {
  return QuoteIdentifier(table.project_id) + "." + QuoteIdentifier(table.dataset_id) + "." +
         QuoteIdentifier(table.table_id);
}

// The DuckDB macro that records a routine in its dataset's schema; see Routine.
inline std::string QualifiedName(const RoutineReference& routine) {
  return QuoteIdentifier(routine.project_id) + "." + QuoteIdentifier(routine.dataset_id) + "." +
         QuoteIdentifier(routine.routine_id);
}

// How BigQuery names `table` in messages: project:dataset.table.
inline std::string TableName(const TableReference& table) {
  return table.project_id + ":" + table.dataset_id + "." + table.table_id;
}

}  // namespace bigquery_emulator_duckdb
