#pragma once

#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"

namespace bigquery_emulator_duckdb {

// A temporary JavaScript UDF. The translator calls it through the bq_js_* backend functions,
// passing `source`: the UDF as a JavaScript function expression over its arguments.
class JavaScriptFunction : public googlesql::Function {
 public:
  JavaScriptFunction(std::vector<std::string> name_path,
                     const googlesql::FunctionSignature& signature,
                     const std::vector<std::string>& argument_names, const std::string& body)
      : googlesql::Function(std::move(name_path), "JavaScript", googlesql::Function::SCALAR,
                            {signature}, VolatileOptions()),
        source_("(function(" + Join(argument_names) + ") {\n" + body + "\n})") {}

  [[nodiscard]] const std::string& source() const { return source_; }

 private:
  // JavaScript can keep state across calls, so calls are never folded or shared.
  static googlesql::FunctionOptions VolatileOptions() {
    googlesql::FunctionOptions options;
    options.set_volatility(googlesql::FunctionEnums::VOLATILE);
    return options;
  }

  static std::string Join(const std::vector<std::string>& names) {
    std::string joined;
    for (const auto& name : names) {
      joined += (joined.empty() ? "" : ", ") + name;
    }
    return joined;
  }

  std::string source_;
};

}  // namespace bigquery_emulator_duckdb
