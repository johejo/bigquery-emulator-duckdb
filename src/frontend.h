#pragma once

#include <memory>
#include <string>

namespace googlesql {
class ASTStatement;
class LanguageOptions;
class ParserOutput;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// A parsed GoogleSQL statement. It owns the parser output, which in turn owns the arena the
// AST nodes live in, so the AST stays valid for as long as the result does.
class FrontendResult {
 public:
  FrontendResult(std::string sql, std::unique_ptr<googlesql::ParserOutput> parser_output);
  FrontendResult(const FrontendResult&) = delete;
  FrontendResult& operator=(const FrontendResult&) = delete;
  FrontendResult(FrontendResult&&) noexcept;
  FrontendResult& operator=(FrontendResult&&) noexcept;
  ~FrontendResult();

  const std::string& sql() const { return sql_; }
  const googlesql::ASTStatement& statement() const;

 private:
  std::string sql_;
  std::unique_ptr<googlesql::ParserOutput> parser_output_;
};

// The language settings shared by the parser and the analyzer, so that a statement the parser
// accepts is not then rejected by the analyzer for a feature it was not told about.
const googlesql::LanguageOptions& GoogleSqlLanguageOptions();

// Parses `sql` as a single GoogleSQL statement. Throws std::runtime_error on a syntax error.
FrontendResult ParseGoogleSql(const std::string& sql);

// Whether the statement is a query or a DML statement (INSERT, UPDATE, DELETE or MERGE), the
// statements the emulator resolves with the analyzer before running them.
bool IsQueryOrDml(const FrontendResult& frontend_result);

}  // namespace bigquery_emulator_duckdb
