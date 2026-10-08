#include "src/routine_catalog.h"

#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/function.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/sql_function.h"
#include "googlesql/public/templated_sql_function.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/analyzer.h"
#include "src/catalog.h"
#include "src/references.h"
#include "src/routine.h"

namespace bigquery_emulator_duckdb {
namespace {

// How a status the analyzer throws begins.
constexpr std::string_view kInvalidArgument = "INVALID_ARGUMENT: ";

}  // namespace

struct RoutineCatalog::State {
  // Declared in the order they are needed: functions borrow the bodies of the analyses and the
  // catalogs that templated bodies are resolved against, and analyses refer to functions.
  std::vector<std::unique_ptr<RoutineCatalog>> catalogs;
  std::vector<AnalyzerResult> analyses;
  std::map<std::vector<std::string>, std::unique_ptr<googlesql::Function>> functions;
  std::set<std::vector<std::string>> resolving;
};

RoutineCatalog::RoutineCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                               std::string default_project, std::string default_dataset,
                               const TemporaryTables* temporary)
    : BigQueryCatalog(source, type_factory, std::move(default_project), std::move(default_dataset),
                      temporary),
      owned_state_(std::make_unique<State>()),
      state_(owned_state_.get()) {}

RoutineCatalog::RoutineCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                               std::string default_project, State* state)
    : BigQueryCatalog(source, type_factory, std::move(default_project), ""), state_(state) {}

RoutineCatalog::~RoutineCatalog() = default;

absl::Status RoutineCatalog::FindFunction(const absl::Span<const std::string>& path,
                                          const googlesql::Function** function,
                                          const FindOptions& options) {
  // Built-in functions come first, NET.HOST among them.
  absl::Status builtin = BigQueryCatalog::FindFunction(path, function, options);
  if (!absl::IsNotFound(builtin)) {
    return builtin;
  }
  const std::vector<std::string> parts = SplitTablePath(path);
  if (parts.size() != 2 && parts.size() != 3) {
    return builtin;
  }
  const std::vector<std::string> key = NormalizeTablePath(parts, default_project(), "");
  if (key.empty()) {
    return builtin;
  }
  if (const auto found = state_->functions.find(key); found != state_->functions.end()) {
    *function = found->second.get();
    return absl::OkStatus();
  }
  if (state_->resolving.contains(key)) {
    return absl::InvalidArgumentError("Recursive calls of function " + absl::StrJoin(key, ".") +
                                      " are not allowed");
  }
  std::optional<Routine> routine = source().FindRoutine({key.at(0), key.at(1), key.at(2)});
  if (!routine.has_value()) {
    return builtin;
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<googlesql::Function> resolved, Resolve(*routine));
  *function = resolved.get();
  state_->functions.emplace(key, std::move(resolved));
  return absl::OkStatus();
}

absl::Status RoutineCatalog::CheckRoutine(const Routine& routine) {
  return Resolve(routine).status();
}

absl::StatusOr<std::unique_ptr<googlesql::Function>> RoutineCatalog::Resolve(
    const Routine& routine) {
  const RoutineReference& reference = routine.reference;
  const std::vector<std::string> key = {
      reference.project_id,
      reference.dataset_id,
      reference.routine_id,
  };
  auto& catalog = state_->catalogs.emplace_back(
      new RoutineCatalog(source(), type_factory(), reference.project_id, state_));
  state_->resolving.insert(key);
  std::optional<AnalyzerResult> analyzed;
  std::string error;
  try {
    analyzed.emplace(AnalyzeGoogleSql(RoutineStatement(routine), *catalog, *type_factory(),
                                      {.default_project = reference.project_id}));
  } catch (const std::exception& exception) {
    // Only the message, without the location and caret that point into the statement the
    // emulator wrote.
    std::string_view message = exception.what();
    message = message.substr(0, message.find('\n'));
    if (message.starts_with(kInvalidArgument)) message.remove_prefix(kInvalidArgument.size());
    error = message.substr(0, message.rfind(" [at "));
  }
  state_->resolving.erase(key);
  if (!analyzed.has_value()) {
    return absl::InvalidArgumentError(error);
  }
  const auto& create = *analyzed->statement().GetAs<googlesql::ResolvedCreateFunctionStmt>();
  std::unique_ptr<googlesql::Function> function;
  if (create.function_expression() != nullptr) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        function,
        googlesql::SQLFunction::Create(create.name_path(), googlesql::Function::SCALAR,
                                       create.signature(), googlesql::FunctionOptions(),
                                       create.function_expression(), create.argument_name_list()));
  } else {
    auto templated = std::make_unique<googlesql::TemplatedSQLFunction>(
        create.name_path(), create.signature(), create.argument_name_list(),
        googlesql::ParseResumeLocation::FromStringView(create.code()));
    templated->set_resolution_catalog(catalog.get());
    function = std::move(templated);
  }
  state_->analyses.push_back(*std::move(analyzed));
  return function;
}

}  // namespace bigquery_emulator_duckdb
