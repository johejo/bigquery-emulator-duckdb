#include "src/python_function.h"

#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/type.h"
#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {
namespace {
googlesql::FunctionOptions VolatileOptions() {
  googlesql::FunctionOptions options;
  options.set_volatility(googlesql::FunctionEnums::VOLATILE);
  return options;
}
}  // namespace

PythonFunction::PythonFunction(std::vector<std::string> name_path,
                               const googlesql::FunctionSignature& signature,
                               const std::string& body, const std::string& entry_point)
    : googlesql::Function(name_path, "Python", googlesql::Function::SCALAR, {signature},
                          VolatileOptions()),
      definition_(nlohmann::json{
          {"name", std::move(name_path)},
          {"body", body},
          {"entry_point", entry_point},
          {"result_type", signature.result_type().type()->TypeName(googlesql::PRODUCT_EXTERNAL)},
      }
                      .dump()) {}

bool PythonFunction::Supports(const googlesql::Type* type) {
  return type != nullptr && (type->IsBool() || type->IsInt64() || type->IsDouble() ||
                             type->IsString() || type->IsBytes());
}
}  // namespace bigquery_emulator_duckdb
