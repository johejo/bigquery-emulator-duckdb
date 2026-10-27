#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/backend_initializer.h"
#include "src/builtin_function_options.h"
#include "src/emulator.h"
#include "src/external_query.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_geography/options.h"
#include "src/project.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {
namespace {

// These tests exercise independent analysis environments, not function results:
// BigQuery semantics are covered by E2E, and GoogleSQL semantics by compliance.
TEST(AnalyzerDeclarationsTest, DeclarationSetsAreIndependentOfExclusions) {
  for (const bool exclude : {false, true}) {
    std::array<std::unique_ptr<Emulator>, 2> emulators;
    for (const auto declarations :
         {FunctionDeclarations::kGoogleSQL, FunctionDeclarations::kBigQuery}) {
      auto emulator = std::make_unique<Emulator>(
          "", std::vector<Project>{{.project_id = "policy"}}, std::nullopt, BackendInitializer{},
          std::vector<ExternalConnection>(), googlesql_geography::Options(),
          functions_duckdb::Options(),
          exclude ? BigQueryExcludedFunctionIds() : ExcludedFunctionIds(), declarations);
      emulator->CreateDataset({.project_id = "policy", .dataset_id = "dataset"});
      emulators.at(declarations == FunctionDeclarations::kBigQuery ? 1 : 0) = std::move(emulator);
    }

    // Revisit each environment after using the other, with the same exclusion list.
    for (const int index : {0, 1, 0, 1}) {
      Emulator& emulator = *emulators.at(index);
      const bool bigquery = index == 1;
      for (const char* sql : {
               "SELECT CONTAINS_SUBSTR('policy', 'policy')",
               "DECLARE present BOOL DEFAULT CONTAINS_SUBSTR('policy', 'policy'); SELECT present",
               "CREATE TEMP FUNCTION present() AS (CONTAINS_SUBSTR('policy', 'policy')); "
               "SELECT present()",
               "CREATE OR REPLACE FUNCTION dataset.present() "
               "AS (CONTAINS_SUBSTR('policy', 'policy'))",
               "CREATE OR REPLACE VIEW dataset.policy_view AS "
               "SELECT CONTAINS_SUBSTR('policy', 'policy') AS present",
           }) {
        SCOPED_TRACE(sql);
        const auto job = emulator.RunQuery({
            .project_id = "policy",
            .location = std::nullopt,
            .query = sql,
            .default_dataset = DatasetReference{.project_id = "policy", .dataset_id = "dataset"},
            .job_id = {},
            .parameters = {},
            .destination_table = std::nullopt,
        });
        EXPECT_EQ(job->error.has_value(), !bigquery)
            << (job->error.has_value() ? job->error->what() : "query succeeded");
        if (!bigquery && job->error.has_value()) {
          EXPECT_NE(std::string(job->error->what()).find("Function not found"), std::string::npos);
        }
      }

      const auto months = emulator.RunQuery({
          .project_id = "policy",
          .location = std::nullopt,
          .query = "SELECT MONTHS_BETWEEN(DATE '2025-06-01', DATE '2025-05-01')",
          .default_dataset = std::nullopt,
          .job_id = {},
          .parameters = {},
          .destination_table = std::nullopt,
      });
      EXPECT_EQ(months->error.has_value(), exclude)
          << (months->error.has_value() ? months->error->what() : "query succeeded");

      // With no external database, BigQuery mode reaches argument validation;
      // GoogleSQL mode must reject the name before trying to describe a query.
      const auto external = emulator.RunQuery({
          .project_id = "policy",
          .location = std::nullopt,
          .query = "SELECT * FROM EXTERNAL_QUERY('invalid', 'SELECT 1')",
          .default_dataset = std::nullopt,
          .job_id = {},
          .parameters = {},
          .destination_table = std::nullopt,
      });
      ASSERT_TRUE(external->error.has_value());
      const std::string message = external->error->what();
      EXPECT_NE(message.find(bigquery ? "Invalid connection ID" : "not found"), std::string::npos)
          << message;
    }
  }
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
