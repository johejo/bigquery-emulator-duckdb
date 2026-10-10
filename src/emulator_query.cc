#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/type.h"
#include "src/analyzer.h"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/catalog.h"
#include "src/ddl_write.h"
#include "src/duckdb_sql.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/routine_catalog.h"
#include "src/translated_statement.h"
#include "src/translator.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

void RoutineError(const TranslatedStatement& translation, const BackendError& error) {
  const std::optional<RoutineReference>& routine = translation.ddl_target_routine;
  if (!routine.has_value()) {
    return;
  }
  const std::string name =
      routine->project_id + ":" + routine->dataset_id + "." + routine->routine_id;
  const std::string_view message = error.what();
  if (message.find("Schema with name") != std::string_view::npos) {
    throw ApiError::NotFound("Not found: Dataset " + routine->project_id + ":" +
                             routine->dataset_id);
  }
  if (message.find("already exists") != std::string_view::npos) {
    throw ApiError::Duplicate("Already Exists: Function " + name);
  }
  if (message.find("does not exist") != std::string_view::npos) {
    throw ApiError::NotFound("Not found: Function " + name);
  }
}

// Resolves the body of the routine a CREATE FUNCTION defines as its calls will, which the
// statement's own analysis does not: against the routine's project, with no default dataset.
void CheckRoutine(const TranslatedStatement& translation, RoutineCatalog& catalog) {
  if (!translation.routine.has_value()) {
    return;
  }
  if (const absl::Status status = catalog.CheckRoutine(translation.routine->routine);
      !status.ok()) {
    throw ApiError::InvalidQuery(std::string(status.message()));
  }
}

const googlesql::Type* ParameterType(const FieldSchema& field,
                                     googlesql::TypeFactory& type_factory) {
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
  if (!type.ok()) {
    throw ApiError::InvalidQuery(std::string(type.status().message()));
  }
  return *type;
}

// Types whose values the backend writes the same way on the wire, so that a column DuckDB
// computed as one of them can be reported as another: all numbers are decimal strings, and
// these textual types are plain strings.
bool SameWireEncoding(FieldType a, FieldType b) {
  static const auto* const kGroups = new std::vector<std::set<FieldType>>{
      {FieldType::kInteger, FieldType::kFloat, FieldType::kNumeric, FieldType::kBigNumeric},
      {FieldType::kString, FieldType::kJson, FieldType::kGeography},
  };
  if (a == b) {
    return true;
  }
  return std::ranges::any_of(*kGroups, [&](const std::set<FieldType>& group) {
    return group.contains(a) && group.contains(b);
  });
}

// Takes the column names, and the types where it can, from the schema the analyzer resolved,
// which follows BigQuery's typing rules (SUM of INT64 is INT64, an unnamed column is f0_), over
// the one DuckDB reports. The rows are still encoded after DuckDB's types, so a type that
// would change their encoding (DATETIME for a TIMESTAMP, say) is left as DuckDB has it.
std::vector<FieldSchema> ReconcileSchema(std::vector<FieldSchema> duckdb_schema,
                                         const std::vector<FieldSchema>& resolved_schema) {
  if (duckdb_schema.size() != resolved_schema.size()) {
    return duckdb_schema;
  }
  for (size_t i = 0; i < duckdb_schema.size(); ++i) {
    FieldSchema& field = duckdb_schema.at(i);
    const FieldSchema& resolved = resolved_schema.at(i);
    field.name = resolved.name;
    if (field.mode != resolved.mode) {
      continue;
    }
    if (field.type == FieldType::kRecord && resolved.type == FieldType::kRecord) {
      field.fields = ReconcileSchema(std::move(field.fields), resolved.fields);
    } else if (SameWireEncoding(field.type, resolved.type)) {
      field.type = resolved.type;
    }
  }
  return duckdb_schema;
}

}  // namespace

QueryResult Emulator::Execute(const std::string& sql, const std::vector<std::string>& setup,
                              bool null_arrays) {
  try {
    return backend_.Execute(sql, setup, null_arrays);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

QueryResult Emulator::Prepare(const std::string& sql, const std::vector<std::string>& setup) {
  try {
    return backend_.Prepare(sql, setup);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

TranslatedStatement Emulator::Translate(const std::string& query, const QueryParameters& parameters,
                                        const std::string& default_project,
                                        const std::string& default_dataset) {
  AnalyzerSettings settings{.default_project = default_project, .default_dataset = default_dataset};
  googlesql::TypeFactory type_factory;
  for (const FieldSchema& field : parameters.named_types()) {
    settings.named_parameters.emplace_back(field.name, ParameterType(field, type_factory));
  }
  for (const FieldSchema& field : parameters.positional_types()) {
    settings.positional_parameters.push_back(ParameterType(field, type_factory));
  }
  const auto source = NewTableSource();
  RoutineCatalog catalog(*source, &type_factory, settings.default_project,
                         settings.default_dataset);
  const AnalyzerResult analyzed = AnalyzeGoogleSql(query, catalog, type_factory, settings);
  std::string unsupported;
  std::optional<TranslatedStatement> translated =
      TranslateStatement(analyzed.statement(), parameters,
                         DefaultDataset{
                             .project = settings.default_project,
                             .dataset = settings.default_dataset,
                             .temporary = nullptr,
                             .has_session_user = has_session_user_,
                         },
                         &unsupported);
  if (!translated.has_value()) {
    throw ApiError::InvalidQuery("The emulator does not support " + unsupported);
  }
  CheckRoutine(*translated, catalog);
  return *std::move(translated);
}

std::shared_ptr<const Job> Emulator::RunQuery(QueryRequest request) {
  request.project_id = ResolveProject(request.project_id);
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->job_id = request.job_id;
  job->configuration = QueryJob{
      .query = request.query,
      .dry_run = request.dry_run,
      .destination_table = request.destination_table,
      .create_disposition = request.create_disposition,
      .write_disposition = request.write_disposition,
  };
  return RunJob(std::move(job), [&](Job& job) {
    if (request.destination_table) {
      auto& project = request.destination_table->project_id;
      project = ResolveProject(project.empty() ? request.project_id : project);
    }
    std::vector<std::string> setup;
    AnalyzerSettings settings{.default_project = request.project_id};
    if (request.default_dataset.has_value()) {
      const std::string dataset_project = ResolveProject(request.default_dataset->project_id.empty()
                                                             ? request.project_id
                                                             : request.default_dataset->project_id);
      setup.push_back("USE " + QualifiedName(DatasetReference{
                                   .project_id = dataset_project,
                                   .dataset_id = request.default_dataset->dataset_id,
                               }));
      settings.default_project = dataset_project;
      settings.default_dataset = request.default_dataset->dataset_id;
    } else {
      setup.push_back("USE " + QuoteIdentifier(request.project_id));
    }

    std::get<QueryJob>(job.configuration).destination_table = request.destination_table;

    if (const auto script = ParseScript(request.query)) {
      std::get<QueryJob>(job.configuration).statement_type = ScriptStatementType(*script);
      job.result =
          RunScript(request, *script, settings.default_project, settings.default_dataset, setup);
      return;
    }
    const TranslatedStatement translation = Translate(
        request.query, request.parameters, settings.default_project, settings.default_dataset);
    auto& query = std::get<QueryJob>(job.configuration);
    query.statement_type = translation.statement_type;
    query.ddl_target_table = translation.ddl_target_table;
    query.ddl_target_dataset = translation.ddl_target_dataset;
    query.ddl_target_routine = translation.ddl_target_routine;
    if (!request.dry_run && !request.destination_table.has_value()) {
      job.result = RunStatement(translation, setup, request.null_arrays);
      return;
    }
    CheckDdlTarget(translation);
    if (request.destination_table.has_value() && !translation.result_schema.has_value()) {
      throw ApiError::Invalid("Cannot set destination table in jobs with DML/DDL statements");
    }
    QueryResult result;
    if (!request.dry_run) {
      result = WriteDestination(request.project_id, *request.destination_table,
                                request.create_disposition, request.write_disposition,
                                translation.sql, *translation.result_schema, setup);
    } else if (!AlterationStatements(translation).has_value()) {
      // A dry run checks an alteration against the table or dataset without running it.
      result = Prepare(translation.sql, setup);
    }
    if (translation.result_schema.has_value()) {
      result.schema = ReconcileSchema(std::move(result.schema), *translation.result_schema);
    }
    job.result = std::move(result);
  });
}

QueryResult Emulator::RunStatement(const TranslatedStatement& translation,
                                   const std::vector<std::string>& setup, bool null_arrays,
                                   Backend* session) {
  Backend& backend = session != nullptr ? *session : backend_;
  CheckDdlTarget(translation);
  QueryResult result;
  if (const std::optional<std::vector<std::string>> statements =
          AlterationStatements(translation)) {
    if (!statements->empty()) {
      backend.ExecuteDdl(statements->front(), {statements->begin() + 1, statements->end()}, "",
                         setup);
    }
  } else if (const std::optional<DdlWrite> write = MetadataWrite(translation)) {
    try {
      backend.ExecuteDdl(translation.sql, write->metadata_statements, write->skip_query, setup);
    } catch (const BackendError& error) {
      RoutineError(translation, error);
      throw;
    }
  } else if (translation.ddl_target_routine.has_value()) {
    try {
      backend.Execute(translation.sql, setup);
    } catch (const BackendError& error) {
      RoutineError(translation, error);
      throw;
    }
  } else {
    result = backend.Execute(translation.sql, setup, null_arrays);
  }
  if (translation.result_schema.has_value()) {
    result.schema = ReconcileSchema(std::move(result.schema), *translation.result_schema);
  }
  return result;
}

}  // namespace bigquery_emulator_duckdb
