#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/analyzer_functions/declarations.h"
#include "src/language_options.h"

namespace bigquery_emulator_duckdb::analyzer_functions {

// OBJ.MAKE_REF and OBJ.FETCH_METADATA, in the obj namespace, over the ObjectRef STRUCT.
// third_party/bigquery_docs/reference/standard-sql/objectref_functions.md
void AddObjectRefFunctions(googlesql::SimpleCatalog& catalog) {
  const googlesql::Type* objectref = nullptr;
  if (!catalog.type_factory()
           ->MakeStructType(
               {
                   {"uri", googlesql::types::StringType()},
                   {"version", googlesql::types::StringType()},
                   {"authorizer", googlesql::types::StringType()},
                   {"details", googlesql::types::JsonType()},
               },
               &objectref)
           .ok()) {
    throw std::runtime_error("failed to make the type of ObjectRef");
  }
  const absl::StatusOr<const googlesql::ArrayType*> objectrefs =
      catalog.type_factory()->MakeArrayType(objectref);
  if (!objectrefs.ok()) {
    throw std::runtime_error(objectrefs.status().ToString());
  }
  // An omitted optional argument is NULL, as an explicit NULL is.
  const auto optional = [](const char* name, googlesql::FunctionEnums::NamedArgumentKind kind,
                           const googlesql::Value& null) {
    googlesql::FunctionArgumentTypeOptions options(googlesql::FunctionArgumentType::OPTIONAL);
    options.set_argument_name(name, kind);
    options.set_default(null);
    return options;
  };
  const googlesql::FunctionArgumentType string(googlesql::types::StringType());
  auto* obj = new googlesql::SimpleCatalog("obj", catalog.type_factory());
  catalog.AddOwnedCatalog(obj);
  obj->AddOwnedFunction(new googlesql::Function(
      std::vector<std::string>{"obj", "make_ref"}, kBigQueryGroup, googlesql::Function::SCALAR,
      {
          googlesql::FunctionSignature(
              googlesql::FunctionArgumentType(objectref),
              {
                  string,
                  googlesql::FunctionArgumentType(
                      googlesql::types::StringType(),
                      optional("authorizer", googlesql::kPositionalOrNamed,
                               googlesql::Value::NullString())),
                  googlesql::FunctionArgumentType(
                      googlesql::types::StringType(),
                      optional("version", googlesql::kNamedOnly, googlesql::Value::NullString())),
                  googlesql::FunctionArgumentType(
                      googlesql::types::JsonType(),
                      optional("details", googlesql::kNamedOnly, googlesql::Value::NullJson())),
              },
              /*context_id=*/static_cast<int64_t>(0)),
          googlesql::FunctionSignature(googlesql::FunctionArgumentType(objectref),
                                       {
                                           googlesql::FunctionArgumentType(objectref),
                                           string,
                                       },
                                       /*context_id=*/static_cast<int64_t>(0)),
          // Declared so that the translator reports the JSON form as unsupported.
          googlesql::FunctionSignature(
              googlesql::FunctionArgumentType(objectref),
              {googlesql::FunctionArgumentType(googlesql::types::JsonType())},
              /*context_id=*/static_cast<int64_t>(0)),
      }));
  // Volatile, since it reads objects that change between calls.
  obj->AddOwnedFunction(new googlesql::Function(
      std::vector<std::string>{"obj", "fetch_metadata"}, kBigQueryGroup,
      googlesql::Function::SCALAR,
      {
          googlesql::FunctionSignature(googlesql::FunctionArgumentType(objectref),
                                       {googlesql::FunctionArgumentType(objectref)},
                                       /*context_id=*/static_cast<int64_t>(0)),
          googlesql::FunctionSignature(googlesql::FunctionArgumentType(*objectrefs),
                                       {googlesql::FunctionArgumentType(*objectrefs)},
                                       /*context_id=*/static_cast<int64_t>(0)),
      },
      googlesql::FunctionOptions().set_volatility(googlesql::FunctionEnums::VOLATILE)));
}

}  // namespace bigquery_emulator_duckdb::analyzer_functions
