#pragma once

#include <string>
#include <vector>

#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"

namespace googlesql {
class Type;
}

namespace bigquery_emulator_duckdb {

// A persistent Python UDF, evaluated by a separate CPython worker rather than SQL inlining.
class PythonFunction : public googlesql::Function {
 public:
  PythonFunction(std::vector<std::string> name_path, const googlesql::FunctionSignature& signature,
                 const std::string& body, const std::string& entry_point);
  [[nodiscard]] const std::string& definition() const { return definition_; }
  static bool Supports(const googlesql::Type* type);

 private:
  std::string definition_;
};

}  // namespace bigquery_emulator_duckdb
