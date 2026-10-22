#pragma once

#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/references.h"

namespace bigquery_emulator_duckdb {

// A persistent SQL UDF, table function or procedure, as the fields of BigQuery's Routine resource
// describe it: routineType, language, arguments, returnType (absent when the function infers it at
// each call), definitionBody, description, creationTime and lastModifiedTime. The emulator records
// `resource` as JSON in the comment of a DuckDB macro named after the routine in its dataset's
// schema, so that the routine lives and dies with the dataset; the macro itself is never called.
// Each query that calls the routine resolves its body anew, as BigQuery does.
struct Routine {
  RoutineReference reference;
  nlohmann::json resource;
};

// The routine a macro comment records, or nothing for a comment the emulator did not write.
std::optional<Routine> ParseRoutineComment(const RoutineReference& reference,
                                           const std::optional<std::string>& comment);

// The statements that record `routine` on its macro, which must exist.
std::vector<std::string> RoutineCommentStatements(const Routine& routine);

// The routines of `dataset` with their comments, by name.
std::string RoutinesQuery(const DatasetReference& dataset);

// The comment of the routine `routine`, a single row if it exists.
std::string RoutineQuery(const RoutineReference& routine);

// The GoogleSQL spelling of a StandardSqlDataType resource.
std::string StandardSqlTypeName(const nlohmann::json& type);

// The GoogleSQL spelling of a StandardSqlTableType resource.
std::string StandardSqlTableTypeName(const nlohmann::json& type);

// The canonical INFORMATION_SCHEMA.ROUTINES DDL of a routine.
std::string RoutineDdl(const Routine& routine);

// The CREATE FUNCTION, CREATE TABLE FUNCTION or CREATE PROCEDURE statement defining `routine`.
// Throws ApiError::Invalid for a resource it cannot spell, such as an unknown type.
std::string RoutineStatement(const Routine& routine);

}  // namespace bigquery_emulator_duckdb
