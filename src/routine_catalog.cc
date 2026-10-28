#include "src/routine_catalog.h"

#include <algorithm>
#include <cstddef>
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
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/function.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/procedure.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/sql_function.h"
#include "googlesql/public/sql_tvf.h"
#include "googlesql/public/sql_view.h"
#include "googlesql/public/templated_sql_function.h"
#include "googlesql/public/types/annotation.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/analyzer.h"
#include "src/api_error.h"
#include "src/builtin_function_options.h"
#include "src/catalog.h"
#include "src/catalog_table.h"
#include "src/language_options.h"
#include "src/procedure_validation.h"
#include "src/python_function.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/table_path.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {
namespace {

// How a status the analyzer throws begins.
constexpr std::string_view kInvalidArgument = "INVALID_ARGUMENT: ";

// A view whose columns have the collations its query gives them, which a SimpleSQLView, whose
// columns have types alone, would drop.
class AnnotatedSqlView : public googlesql::SQLView {
 public:
  AnnotatedSqlView(std::string name, bool is_value_table, const googlesql::ResolvedScan* query)
      : name_(std::move(name)), is_value_table_(is_value_table), query_(query) {}

  void AddColumn(const std::string& name, googlesql::AnnotatedType type) {
    columns_.push_back(std::make_unique<googlesql::SimpleColumn>(name_, name, type));
  }

  [[nodiscard]] SqlSecurity sql_security() const override { return kSecurityInvoker; }
  [[nodiscard]] const googlesql::ResolvedScan* view_query() const override { return query_; }
  [[nodiscard]] std::string Name() const override { return name_; }
  [[nodiscard]] std::string FullName() const override { return name_; }
  [[nodiscard]] int NumColumns() const override { return static_cast<int>(columns_.size()); }
  [[nodiscard]] const googlesql::Column* GetColumn(int i) const override {
    return columns_.at(static_cast<size_t>(i)).get();
  }
  [[nodiscard]] const googlesql::Column* FindColumnByName(const std::string& name) const override {
    const auto column = std::ranges::find_if(
        columns_, [&](const auto& column) { return absl::EqualsIgnoreCase(column->Name(), name); });
    return column == columns_.end() ? nullptr : column->get();
  }
  [[nodiscard]] bool IsValueTable() const override { return is_value_table_; }

 private:
  std::string name_;
  bool is_value_table_;
  const googlesql::ResolvedScan* query_;
  std::vector<std::unique_ptr<googlesql::SimpleColumn>> columns_;
};

}  // namespace

struct RoutineCatalog::State {
  // Declared in the order they are needed: functions borrow the bodies of the analyses and the
  // catalogs that templated bodies are resolved against, and analyses refer to functions.
  std::vector<std::unique_ptr<RoutineCatalog>> catalogs;
  std::vector<AnalyzerResult> analyses;
  std::map<std::vector<std::string>, std::unique_ptr<googlesql::Function>> functions;
  std::map<std::vector<std::string>, std::unique_ptr<googlesql::Table>> views;
  std::map<std::vector<std::string>, std::unique_ptr<googlesql::TableValuedFunction>> tvfs;
  std::map<std::vector<std::string>, std::unique_ptr<googlesql::Procedure>> procedures;
  std::set<std::vector<std::string>> resolving;
};

RoutineCatalog::RoutineCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                               std::string default_project, std::string default_dataset,
                               const TemporaryTables* temporary,
                               const ExcludedFunctionIds& excluded_function_ids,
                               FunctionDeclarations declarations)
    : BigQueryCatalog(source, type_factory, std::move(default_project), std::move(default_dataset),
                      temporary, excluded_function_ids, declarations),
      owned_state_(std::make_unique<State>()),
      state_(owned_state_.get()) {}

RoutineCatalog::RoutineCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                               std::string default_project, std::string default_dataset,
                               State& state, const ExcludedFunctionIds& excluded_function_ids,
                               FunctionDeclarations declarations)
    : BigQueryCatalog(source, type_factory, std::move(default_project), std::move(default_dataset),
                      nullptr, excluded_function_ids, declarations),
      state_(&state) {}

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
  const std::vector<std::string> key = NormalizeTablePath(parts, default_project(), "", &source());
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
  std::optional<Routine> routine = source().FindRoutine(
      {.project_id = key.at(0), .dataset_id = key.at(1), .routine_id = key.at(2)});
  if (!routine.has_value() || routine->resource.at("routineType") != "SCALAR_FUNCTION") {
    return builtin;
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<googlesql::Function> resolved, Resolve(*routine));
  *function = resolved.get();
  state_->functions.emplace(key, std::move(resolved));
  return absl::OkStatus();
}

absl::Status RoutineCatalog::FindProcedure(const absl::Span<const std::string>& path,
                                           const googlesql::Procedure** procedure,
                                           const FindOptions& /*options*/) {
  const auto key = NormalizeTablePath(SplitTablePath(path), default_project(), "", &source());
  if (key.size() != 3) {
    return absl::NotFoundError("Procedure name must include a dataset");
  }
  if (const auto found = state_->procedures.find(key); found != state_->procedures.end()) {
    *procedure = found->second.get();
    return absl::OkStatus();
  }
  const auto routine = source().FindRoutine(
      {.project_id = key.at(0), .dataset_id = key.at(1), .routine_id = key.at(2)});
  if (!routine || routine->resource.at("routineType") != "PROCEDURE") {
    return absl::NotFoundError("Procedure is not found");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const auto analyzed,
                             AnalyzeDefinition(RoutineStatement(*routine), key, key.at(0), ""));
  const auto& create = *analyzed.statement().GetAs<googlesql::ResolvedCreateProcedureStmt>();
  auto resolved = std::make_unique<googlesql::Procedure>(key, create.signature());
  *procedure = resolved.get();
  state_->procedures.emplace(key, std::move(resolved));
  return absl::OkStatus();
}

absl::Status RoutineCatalog::CheckRoutine(const Routine& routine) {
  if (routine.resource.at("routineType") == "PROCEDURE") {
    // BigQuery rejects query parameters in the body even without strict mode.
    std::unique_ptr<googlesql::ParserOutput> parsed;
    GOOGLESQL_RETURN_IF_ERROR(
        googlesql::ParseScript(routine.resource.at("definitionBody").get<std::string>(),
                               googlesql::ParserOptions(GoogleSqlLanguageOptions()),
                               {.mode = googlesql::ERROR_MESSAGE_ONE_LINE}, &parsed));
    std::vector<const googlesql::ASTNode*> nodes{parsed->script()};
    while (!nodes.empty()) {
      const googlesql::ASTNode* node = nodes.back();
      nodes.pop_back();
      if (const auto* parameter = node->GetAsOrNull<googlesql::ASTParameterExpr>()) {
        if (parameter->name() == nullptr) {
          return absl::UnimplementedError(
              absl::StrCat(kUnsupported, "positional query parameters in procedure bodies"));
        }
        return absl::InvalidArgumentError(
            absl::StrCat("Query parameter is not allowed in the body of procedure '",
                         routine.reference.dataset_id, ".", routine.reference.routine_id, "': @",
                         parameter->name()->GetAsStringView()));
      }
      for (int i = 0; i < node->num_children(); ++i) {
        nodes.push_back(node->child(i));
      }
    }
    if (routine.resource.value("strictMode", true)) {
      RoutineCatalog catalog(source(), type_factory(), routine.reference.project_id, "", nullptr,
                             excluded_function_ids(), function_declarations());
      return CheckProcedure(routine, *parsed->script(), catalog, *type_factory());
    }
    return absl::OkStatus();
  }
  if (routine.resource.at("routineType") == "TABLE_VALUED_FUNCTION") {
    return ResolveTableFunction(routine).status();
  }
  return Resolve(routine).status();
}

absl::Status RoutineCatalog::CheckView(const TableReference& reference, const std::string& query) {
  return AnalyzeDefinition(query,
                           {"", reference.project_id, reference.dataset_id, reference.table_id},
                           reference.project_id, reference.dataset_id)
      .status();
}

absl::StatusOr<std::unique_ptr<googlesql::Function>> RoutineCatalog::Resolve(
    const Routine& routine) {
  const RoutineReference& reference = routine.reference;
  const std::vector<std::string> key = {
      reference.project_id,
      reference.dataset_id,
      reference.routine_id,
  };
  RoutineCatalog* catalog = nullptr;
  GOOGLESQL_ASSIGN_OR_RETURN(auto analyzed, AnalyzeDefinition(RoutineStatement(routine), key,
                                                              reference.project_id, "", &catalog));
  const auto& create = *analyzed.statement().GetAs<googlesql::ResolvedCreateFunctionStmt>();
  std::unique_ptr<googlesql::Function> function;
  if (routine.resource.value("language", "SQL") == "PYTHON") {
    return std::unique_ptr<googlesql::Function>(std::make_unique<PythonFunction>(
        key, create.signature(), create.code(),
        routine.resource.at("pythonOptions").at("entryPoint").get<std::string>()));
  }
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
    templated->set_resolution_catalog(catalog);
    function = std::move(templated);
  }
  state_->analyses.push_back(std::move(analyzed));
  return function;
}

absl::StatusOr<AnalyzerResult> RoutineCatalog::AnalyzeDefinition(
    const std::string& sql, const std::vector<std::string>& key, const std::string& project,
    const std::string& dataset, RoutineCatalog** body_catalog) {
  if (!state_->resolving.insert(key).second) {
    return absl::InvalidArgumentError("Recursive dependency: " + absl::StrJoin(key, "."));
  }
  auto* catalog = new RoutineCatalog(source(), type_factory(), project, dataset, *state_,
                                     excluded_function_ids(), function_declarations());
  state_->catalogs.emplace_back(catalog);
  if (body_catalog != nullptr) {
    *body_catalog = catalog;
  }
  std::optional<AnalyzerResult> analyzed;
  std::string error;
  try {
    analyzed.emplace(AnalyzeGoogleSql(sql, *catalog, *type_factory(),
                                      {.default_project = project, .default_dataset = dataset}));
  } catch (const std::exception& exception) {
    std::string_view message = exception.what();
    message = message.substr(0, message.find('\n'));
    if (message.starts_with(kInvalidArgument)) {
      message.remove_prefix(kInvalidArgument.size());
    }
    error = message.substr(0, message.rfind(" [at "));
  }
  state_->resolving.erase(key);
  if (!analyzed.has_value()) {
    return absl::InvalidArgumentError(error);
  }
  return *std::move(analyzed);
}

absl::Status RoutineCatalog::FindTable(const absl::Span<const std::string>& path,
                                       const googlesql::Table** table, const FindOptions& options) {
  GOOGLESQL_RETURN_IF_ERROR(BigQueryCatalog::FindTable(path, table, options));
  const auto* stored = dynamic_cast<const BigQueryTable*>(*table);
  if (stored == nullptr) {
    return absl::OkStatus();
  }
  const TableReference reference = stored->reference();
  const std::vector<std::string> key = {
      "",
      reference.project_id,
      reference.dataset_id,
      reference.table_id,
  };
  if (state_->resolving.contains(key)) {
    return absl::InvalidArgumentError("Recursive view dependency: " + stored->FullName());
  }
  if (const auto found = state_->views.find(key); found != state_->views.end()) {
    *table = found->second.get();
    return absl::OkStatus();
  }
  const auto view =
      source().FindView(reference.project_id, reference.dataset_id, reference.table_id);
  if (!view || view->query.empty()) {
    return absl::OkStatus();
  }
  if (view->materialized_source) {
    const auto& base = *view->materialized_source;
    const auto current =
        source().DescribeTable(base.table.project_id, base.table.dataset_id, base.table.table_id);
    if (!current || current->metadata.creation_time != base.creation_time) {
      return absl::InvalidArgumentError(
          "The base table of materialized view " + stored->FullName() +
          " was deleted or recreated; recreate the materialized view");
    }
  }
  auto analyzed = AnalyzeDefinition(view->query, key, reference.project_id, reference.dataset_id);
  if (!analyzed.ok()) {
    return absl::InvalidArgumentError(std::string(analyzed.status().message()) +
                                      "; failed to parse view '" + reference.dataset_id + "." +
                                      reference.table_id + "'");
  }
  const auto& query = *analyzed->statement().GetAs<googlesql::ResolvedQueryStmt>();
  auto resolved =
      std::make_unique<AnnotatedSqlView>(stored->FullName(), query.is_value_table(), query.query());
  for (int i = 0; i < query.output_column_list_size(); ++i) {
    const auto* output = query.output_column_list(i);
    resolved->AddColumn(i < stored->NumColumns() ? stored->GetColumn(i)->Name() : output->name(),
                        output->column().annotated_type());
  }
  *table = resolved.get();
  state_->views.emplace(key, std::move(resolved));
  state_->analyses.push_back(*std::move(analyzed));
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<googlesql::TableValuedFunction>>
RoutineCatalog::ResolveTableFunction(const Routine& routine) {
  const auto& reference = routine.reference;
  GOOGLESQL_ASSIGN_OR_RETURN(
      auto analyzed,
      AnalyzeDefinition(RoutineStatement(routine),
                        {reference.project_id, reference.dataset_id, reference.routine_id},
                        reference.project_id, ""));
  std::unique_ptr<googlesql::SQLTableValuedFunction> function;
  GOOGLESQL_RETURN_IF_ERROR(googlesql::SQLTableValuedFunction::Create(
      analyzed.statement().GetAs<googlesql::ResolvedCreateTableFunctionStmt>(), &function));
  state_->analyses.push_back(std::move(analyzed));
  return function;
}

absl::Status RoutineCatalog::FindTableValuedFunction(
    const absl::Span<const std::string>& path, const googlesql::TableValuedFunction** function,
    const FindOptions& options) {
  absl::Status builtin = BigQueryCatalog::FindTableValuedFunction(path, function, options);
  if (!absl::IsNotFound(builtin)) {
    return builtin;
  }
  const auto parts = SplitTablePath(path);
  if (parts.size() != 2 && parts.size() != 3) {
    return builtin;
  }
  const auto key = NormalizeTablePath(parts, default_project(), "", &source());
  if (key.empty()) {
    return builtin;
  }
  if (state_->resolving.contains(key)) {
    return absl::InvalidArgumentError("Recursive calls of table function " +
                                      absl::StrJoin(key, ".") + " are not allowed");
  }
  if (const auto found = state_->tvfs.find(key); found != state_->tvfs.end()) {
    *function = found->second.get();
    return absl::OkStatus();
  }
  const auto routine = source().FindRoutine(
      {.project_id = key.at(0), .dataset_id = key.at(1), .routine_id = key.at(2)});
  if (!routine || routine->resource.at("routineType") != "TABLE_VALUED_FUNCTION") {
    return builtin;
  }
  GOOGLESQL_ASSIGN_OR_RETURN(auto resolved, ResolveTableFunction(*routine));
  *function = resolved.get();
  state_->tvfs.emplace(key, std::move(resolved));
  return absl::OkStatus();
}

}  // namespace bigquery_emulator_duckdb
