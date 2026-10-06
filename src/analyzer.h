#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/scripting/type_aliases.h"
#include "src/field_schema.h"

namespace googlesql {
class AnalyzerOutput;
class Catalog;
class ResolvedExpr;
class ResolvedStatement;
class ScriptExecutor;
class ScriptSegment;
class Type;
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

struct AnalyzerSettings {
  // Used to complete table paths that name no project or dataset.
  std::string default_project;
  std::string default_dataset = {};
  // The types of the query parameters; a statement uses either named or positional ones.
  std::vector<std::pair<std::string, const googlesql::Type*>> named_parameters = {};
  std::vector<const googlesql::Type*> positional_parameters = {};
  // The script that a statement or expression belongs to, whose system variables, such as
  // @@error.message, the analyzer then knows. Its variables are constants of the catalog.
  const googlesql::ScriptExecutor* script = nullptr;
};

// The result column for field `index` (0-based) of a query returning a value table of structs,
// whose fields BigQuery returns as columns; anonymous fields are named `_field_<index + 1>`.
std::string ValueTableFieldName(const std::string& name, int index);

// A resolved GoogleSQL statement. It owns the analyzer output, which owns the resolved AST, so
// the statement stays valid for as long as the result does. Types in the AST come from the
// TypeFactory passed to AnalyzeGoogleSql and are valid only as long as that factory is.
// References to catalog objects (such as tables and functions) likewise require the catalog
// to outlive the result.
class AnalyzerResult {
 public:
  explicit AnalyzerResult(std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output);
  AnalyzerResult(const AnalyzerResult&) = delete;
  AnalyzerResult& operator=(const AnalyzerResult&) = delete;
  AnalyzerResult(AnalyzerResult&&) noexcept;
  AnalyzerResult& operator=(AnalyzerResult&&) noexcept;
  ~AnalyzerResult();

  // The statement, or the expression, that was analyzed.
  const googlesql::ResolvedStatement& statement() const;
  const googlesql::ResolvedExpr& expression() const;

  // ResultSchema of the statement.
  std::optional<std::vector<FieldSchema>> result_schema() const;

 private:
  std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output_;
};

// The schema of the rows a query returns, named and typed the way BigQuery reports them: a
// column without a name is f0_, f1_, ... in the order of the unnamed columns. Empty for a
// statement that is not a query and for a query with a column BigQuery cannot describe.
std::optional<std::vector<FieldSchema>> ResultSchema(const googlesql::ResolvedStatement& statement);

// Parses `sql` as a single GoogleSQL statement and resolves its names and types against
// `catalog`. Throws std::runtime_error with a caret-annotated message when the statement does
// not parse or analyze.
AnalyzerResult AnalyzeGoogleSql(const std::string& sql, googlesql::Catalog& catalog,
                                googlesql::TypeFactory& type_factory,
                                const AnalyzerSettings& settings = {});

// The options the statements of a script are analyzed with, from which its executor takes the
// language, the query parameters and how errors are reported.
absl::StatusOr<googlesql::AnalyzerOptions> ScriptAnalyzerOptions(const AnalyzerSettings& settings);

// Analyzes the type name `segment` of a script's DECLARE.
absl::StatusOr<googlesql::TypeWithParameters> AnalyzeScriptType(
    const googlesql::ScriptSegment& segment, googlesql::Catalog& catalog,
    googlesql::TypeFactory& type_factory, const AnalyzerSettings& settings);

// Analyzes the statement `segment` of a script. A failure is returned rather than thrown, with
// its location in the whole script as a payload, which the script's executor reports.
absl::StatusOr<AnalyzerResult> AnalyzeScriptStatement(const googlesql::ScriptSegment& segment,
                                                      googlesql::Catalog& catalog,
                                                      googlesql::TypeFactory& type_factory,
                                                      const AnalyzerSettings& settings);

// Analyzes the expression `sql` of a script, coerced to `target_type` as an assignment would be,
// or of its own type when `target_type` is null. A failure is located in the whole script when
// `sql` is the text of `segment`, and in `sql` otherwise.
absl::StatusOr<AnalyzerResult> AnalyzeScriptExpression(std::string_view sql,
                                                       const googlesql::ScriptSegment* segment,
                                                       const googlesql::Type* target_type,
                                                       googlesql::Catalog& catalog,
                                                       googlesql::TypeFactory& type_factory,
                                                       const AnalyzerSettings& settings);

}  // namespace bigquery_emulator_duckdb
