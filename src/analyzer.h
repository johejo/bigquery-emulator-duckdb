#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/frontend.h"

namespace googlesql {
class AnalyzerOutput;
class Catalog;
class ResolvedStatement;
class Type;
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

struct AnalyzerSettings {
  // Used to complete table paths that name no project or dataset.
  std::string default_project;
  std::string default_dataset;
  // The types of the query parameters; a statement uses either named or positional ones.
  std::vector<std::pair<std::string, const googlesql::Type*>> named_parameters;
  std::vector<const googlesql::Type*> positional_parameters;
};

// A resolved GoogleSQL statement. It owns the analyzer output, which owns the resolved AST, so
// the statement stays valid for as long as the result does. Types in the AST come from the
// TypeFactory passed to AnalyzeGoogleSql and are valid only as long as that factory is.
class AnalyzerResult {
 public:
  explicit AnalyzerResult(std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output);
  AnalyzerResult(const AnalyzerResult&) = delete;
  AnalyzerResult& operator=(const AnalyzerResult&) = delete;
  AnalyzerResult(AnalyzerResult&&) noexcept;
  AnalyzerResult& operator=(AnalyzerResult&&) noexcept;
  ~AnalyzerResult();

  const googlesql::ResolvedStatement& statement() const;

 private:
  std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output_;
};

// Resolves the names and types of a parsed statement against `catalog`. Throws
// std::runtime_error with a caret-annotated message when the statement does not analyze, the
// same way ParseGoogleSql reports a syntax error.
AnalyzerResult AnalyzeGoogleSql(const FrontendResult& frontend_result, googlesql::Catalog& catalog,
                                googlesql::TypeFactory& type_factory,
                                const AnalyzerSettings& settings = {});

}  // namespace bigquery_emulator_duckdb
