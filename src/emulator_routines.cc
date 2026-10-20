#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/type.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/catalog.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/emulator.h"
#include "src/language_options.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/table_path.h"
#include "src/table_source.h"
#include "src/translated_statement.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

namespace {

// Routine.definitionBody requires project-qualified references to other routines. Built-in
// namespaces such as NET and SAFE are still allowed.
void CheckApiRoutineBody(const Routine& routine, TableSource& source) {
  std::unique_ptr<googlesql::ParserOutput> parsed;
  const bool procedure = routine.resource.at("routineType") == "PROCEDURE";
  const bool tvf = routine.resource.at("routineType") == "TABLE_VALUED_FUNCTION";
  const std::string body = routine.resource.at("definitionBody").get<std::string>();
  const googlesql::ParserOptions options(GoogleSqlLanguageOptions());
  const auto status = procedure
                          ? googlesql::ParseScript(
                                body, options, {.mode = googlesql::ERROR_MESSAGE_ONE_LINE}, &parsed)
                      : tvf ? googlesql::ParseStatement(body, options, &parsed)
                            : googlesql::ParseExpression(body, options, &parsed);
  if (!status.ok()) {
    throw ApiError::Invalid(std::string(status.message()));
  }
  googlesql::TypeFactory types;
  BigQueryCatalog builtins(source, &types, "", "");
  std::vector<const googlesql::ASTNode*> nodes{
      procedure ? static_cast<const googlesql::ASTNode*>(parsed->script())
      : tvf     ? static_cast<const googlesql::ASTNode*>(parsed->statement())
                : parsed->expression(),
  };
  while (!nodes.empty()) {
    const auto* node = nodes.back();
    nodes.pop_back();
    if (const auto* call = node->GetAsOrNull<googlesql::ASTCallStatement>();
        call != nullptr &&
        SplitTablePath(call->procedure_name()->ToIdentifierVector()).size() != 3) {
      throw ApiError::Unsupported("API procedure bodies with CALL references without a project ID");
    }
    if (const auto* call = node->GetAsOrNull<googlesql::ASTFunctionCall>()) {
      std::vector<std::string> path = SplitTablePath(call->function()->ToIdentifierVector());
      if (!path.empty() && ToLowerAscii(path.front()) == "safe") {
        path.erase(path.begin());
      }
      const googlesql::Function* function = nullptr;
      if (path.size() == 2 && !builtins.FindFunction(path, &function).ok()) {
        throw ApiError::Unsupported(
            "API routine bodies with function references without a "
            "project ID");
      }
    }
    for (int i = 0; i < node->num_children(); ++i) {
      nodes.push_back(node->child(i));
    }
  }
}

}  // namespace

std::vector<Routine> Emulator::ListRoutines(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  std::vector<Routine> routines;
  for (const json& row : Execute(RoutinesQuery(dataset)).rows) {
    const RoutineReference reference{
        .project_id = dataset.project_id,
        .dataset_id = dataset.dataset_id,
        .routine_id = row["f"][0]["v"].get<std::string>(),
    };
    if (std::optional<Routine> routine = ParseRoutineComment(reference, row["f"][1]["v"])) {
      routines.push_back(*std::move(routine));
    }
  }
  return routines;
}

Routine Emulator::GetRoutine(RoutineReference routine) {
  routine.project_id = ResolveProject(routine.project_id);
  GetDataset(DatasetReference{.project_id = routine.project_id, .dataset_id = routine.dataset_id});
  const QueryResult result = Execute(RoutineQuery(routine));
  std::optional<Routine> found = result.rows.empty()
                                     ? std::nullopt
                                     : ParseRoutineComment(routine, result.rows.at(0)["f"][0]["v"]);
  if (!found.has_value()) {
    throw ApiError::NotFound("Not found: Routine " + routine.project_id + ":" + routine.dataset_id +
                             "." + routine.routine_id);
  }
  return *std::move(found);
}

Routine Emulator::WriteRoutine(Routine routine, bool update) {
  routine.reference.project_id = ResolveProject(routine.reference.project_id);
  const RoutineReference& reference = routine.reference;
  GetDataset({.project_id = reference.project_id, .dataset_id = reference.dataset_id});
  std::optional<Routine> previous;
  if (update) {
    previous = GetRoutine(reference);
    if (previous->resource.at("routineType") != routine.resource.at("routineType")) {
      throw ApiError::Unsupported("changing a routine's type");
    }
  }
  try {
    const auto source = NewTableSource();
    CheckApiRoutineBody(routine, *source);
    std::string statement = RoutineStatement(routine);
    if (update) {
      statement.replace(0, std::string("CREATE").size(), "CREATE OR REPLACE");
    }
    const TranslatedStatement translation = Translate(statement, {}, reference.project_id, "");
    if (!translation.routine.has_value()) {
      throw ApiError::Invalid("Invalid routine definition");
    }
    const json& created = translation.routine->routine.resource;
    // Keep the API's body verbatim; the analyzer's code includes the newline we append to it.
    routine.resource["creationTime"] =
        previous.has_value() ? previous->resource.at("creationTime") : created.at("creationTime");
    routine.resource["lastModifiedTime"] = created.at("lastModifiedTime");
    backend_.ExecuteDdl(translation.sql, RoutineCommentStatements(routine), "");
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Routine " + reference.project_id + ":" +
                                reference.dataset_id + "." + reference.routine_id);
    }
    throw ApiError::Invalid(error.what());
  } catch (const std::exception& error) {
    throw ApiError::Invalid(error.what());
  }
  return routine;
}

void Emulator::DeleteRoutine(RoutineReference routine) {
  routine.project_id = ResolveProject(routine.project_id);
  GetRoutine(routine);
  Execute("DROP MACRO " + QualifiedName(routine));
}

}  // namespace bigquery_emulator_duckdb
