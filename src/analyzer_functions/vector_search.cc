#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/analyzer_functions/declarations.h"
#include "src/language_options.h"

namespace bigquery_emulator_duckdb::analyzer_functions {
namespace {

// The batch and single forms' signatures.
constexpr int64_t kBatch = 0;
constexpr int64_t kSingle = 1;

// The column `name` selects, ignoring case as BigQuery does, or -1 if none or several do.
int FindColumn(const googlesql::TVFRelation& relation, const std::string& name) {
  int found = -1;
  for (int i = 0; i < relation.num_columns(); ++i) {
    if (absl::EqualsIgnoreCase(relation.column(i).name, name)) {
      if (found != -1) {
        return -1;
      }
      found = i;
    }
  }
  return found;
}

bool IsEmbedding(const googlesql::Type* type) {
  return type->IsArray() && type->AsArray()->element_type()->IsDouble();
}

googlesql::FunctionArgumentType Named(
    const googlesql::Type* type, const std::string& name, googlesql::Value default_value,
    googlesql::FunctionArgumentTypeOptions::NamedArgumentKind kind = googlesql::kNamedOnly) {
  googlesql::FunctionArgumentTypeOptions options(googlesql::FunctionArgumentType::OPTIONAL);
  options.set_argument_name(name, kind);
  options.set_default(std::move(default_value));
  return {type, options};
}

googlesql::FunctionSignature Signature(int64_t form) {
  googlesql::FunctionArgumentTypeOptions column_to_search;
  column_to_search.set_argument_name("column_to_search", googlesql::kPositionalOnly);
  std::vector<googlesql::FunctionArgumentType> arguments = {
      googlesql::FunctionArgumentType::AnyRelation(),
      {googlesql::types::StringType(), column_to_search},
  };
  if (form == kBatch) {
    arguments.push_back(googlesql::FunctionArgumentType::AnyRelation());
    arguments.push_back(Named(googlesql::types::StringType(), "query_column_to_search",
                              googlesql::Value::NullString(), googlesql::kPositionalOrNamed));
  } else {
    // Any type, so that the catalog gives BigQuery's error for other types.
    googlesql::FunctionArgumentTypeOptions query_value;
    query_value.set_argument_name("query_value", googlesql::kNamedOnly);
    arguments.emplace_back(googlesql::ARG_KIND_EXPR_ARBITRARY, query_value);
  }
  // The batch form takes the lexical search arguments only to give BigQuery's error.
  arguments.push_back(Named(googlesql::types::StringArrayType(), "lexical_search_columns",
                            googlesql::Value::Null(googlesql::types::StringArrayType())));
  arguments.push_back(Named(googlesql::types::StringType(), "lexical_search_query_value",
                            googlesql::Value::NullString()));
  arguments.push_back(Named(googlesql::types::Int64Type(), "top_k", googlesql::Value::Int64(10)));
  arguments.push_back(Named(googlesql::types::StringType(), "distance_type",
                            googlesql::Value::String("EUCLIDEAN")));
  arguments.push_back(
      Named(googlesql::types::StringType(), "options", googlesql::Value::String("{}")));
  return {googlesql::ARG_KIND_RELATION, arguments, form};
}

absl::Status Unsupported(absl::string_view what) {
  return absl::InvalidArgumentError(
      absl::StrCat("The emulator does not support VECTOR_SEARCH ", what));
}

// The scalar argument named `name`. Defaults are literals.
const googlesql::TVFInputArgumentType* Argument(
    const googlesql::FunctionSignature& signature,
    const std::vector<googlesql::TVFInputArgumentType>& arguments, absl::string_view name) {
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const auto& argument = signature.ConcreteArgument(static_cast<int>(i));
    if (argument.has_argument_name() && argument.argument_name() == name) {
      return &arguments.at(i);
    }
  }
  return nullptr;
}

// The literal value of a named argument, or nullopt for another expression.
std::optional<googlesql::Value> Literal(const googlesql::TVFInputArgumentType* argument) {
  if (argument == nullptr) {
    return std::nullopt;
  }
  const auto scalar = argument->GetScalarArgType();
  if (!scalar.ok() || scalar->literal_value() == nullptr) {
    return std::nullopt;
  }
  return *scalar->literal_value();
}

// Only embeddings are searched: STRING and ObjectRef columns need autonomous embedding
// generation, which the emulator does not have.
absl::Status CheckBaseColumn(const googlesql::TVFRelation& relation, const std::string& name) {
  const int index = FindColumn(relation, name);
  if (index == -1) {
    return absl::InvalidArgumentError(
        absl::StrCat("Column ", name, " should exist in the 1st argument of VECTOR_SEARCH."));
  }
  const auto& column = relation.column(index);
  if (IsEmbedding(column.type)) {
    return absl::OkStatus();
  }
  if (column.type->IsString() || column.type->IsStruct()) {
    return Unsupported("with autonomous embedding generation");
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "column_to_search argument of VECTOR_SEARCH: ", column.name,
      " should be of type ARRAY<FLOAT64>, STRING, ObjectRef, or STRUCT<ARRAY<FLOAT64>, STRING>."));
}

// The query column BigQuery searches with: the one named, or else the ARRAY<FLOAT64> column named
// like column_to_search, or else the only ARRAY<FLOAT64> column.
absl::Status CheckQueryColumn(const googlesql::TVFRelation& relation, const googlesql::Value& named,
                              const std::string& column_to_search) {
  if (!named.is_null()) {
    const int index = FindColumn(relation, named.string_value());
    if (index == -1) {
      return absl::InvalidArgumentError(
          absl::StrCat(named.string_value(), " should exist in 3rd argument of VECTOR_SEARCH."));
    }
    const auto& column = relation.column(index);
    if (column.type->IsString() || column.type->IsStruct()) {
      return Unsupported("with autonomous embedding generation");
    }
    if (!IsEmbedding(column.type)) {
      return absl::InvalidArgumentError(
          absl::StrCat("query_column_to_search argument of VECTOR_SEARCH: ", column.name,
                       " should be of type ARRAY<FLOAT64>."));
    }
    return absl::OkStatus();
  }
  if (const int index = FindColumn(relation, column_to_search);
      index != -1 && IsEmbedding(relation.column(index).type)) {
    return absl::OkStatus();
  }
  if (std::ranges::count_if(relation.columns(),
                            [](const auto& column) { return IsEmbedding(column.type); }) != 1) {
    return absl::InvalidArgumentError(absl::StrCat(
        "One and only one column of ARRAY<FLOAT64> type should exist in 3rd argument of "
        "VECTOR_SEARCH when query_column_to_search is not specified and no ARRAY<FLOAT64> column "
        "with the same name as ",
        column_to_search, " exists in the 3rd argument."));
  }
  return absl::OkStatus();
}

// BigQuery's options select how a vector index is searched. The emulator has no vector indexes,
// so every search is the exact brute-force search that BigQuery falls back to without one.
absl::Status CheckOptions(const std::string& text) {
  const auto malformed = [&] {
    return absl::InvalidArgumentError(absl::StrCat("Malformated options: ", text));
  };
  const auto parsed = googlesql::JSONValue::ParseJSONString(text);
  if (!parsed.ok() || !parsed->GetConstRef().IsObject()) {
    return malformed();
  }
  // Whether use_brute_force is set: true, or any string but "false" and the malformed "", and
  // whether fraction_lists_to_search is given other than as the null that BigQuery ignores.
  bool brute_force = false;
  bool fraction = false;
  for (const auto& [name, value] : parsed->GetConstRef().GetMembers()) {
    if (name == "use_brute_force") {
      // BigQuery also takes the boolean as any string.
      if (value.IsBoolean()) {
        brute_force = value.GetBoolean();
      } else if (value.IsString()) {
        if (value.GetString().empty()) {
          return malformed();
        }
        brute_force = value.GetString() != "false";
      } else {
        return malformed();
      }
    } else if (name == "fraction_lists_to_search" && !value.IsNull()) {
      // BigQuery also takes the number as a string, even NaN, which is in no range it rejects.
      double number = 0;
      if (value.IsString()) {
        if (!absl::SimpleAtod(value.GetString(), &number)) {
          return malformed();
        }
      } else if (!value.IsNumber()) {
        return malformed();
      } else {
        number = value.IsInt64()    ? static_cast<double>(value.GetInt64())
                 : value.IsUInt64() ? static_cast<double>(value.GetUInt64())
                                    : value.GetDouble();
      }
      if (number <= 0 || number > 1) {
        return absl::InvalidArgumentError(
            absl::StrCat("fraction_lists_to_search in options of VECTOR_SEARCH should be in the "
                         "range of (0, 1]. Got ",
                         number, "."));
      }
      fraction = true;
    } else if (name != "fraction_lists_to_search") {
      return malformed();
    }
  }
  if (fraction && brute_force) {
    return absl::InvalidArgumentError(
        "fraction_lists_to_search should not be specified when use_brute_force is set.");
  }
  return absl::OkStatus();
}

absl::StatusOr<googlesql::TVFRelation::Column> StructColumn(googlesql::TypeFactory& type_factory,
                                                            const googlesql::TVFRelation& relation,
                                                            const std::string& name) {
  if (relation.is_value_table()) {
    return Unsupported("over a value table");
  }
  std::vector<googlesql::StructField> fields;
  std::ranges::transform(relation.columns(), std::back_inserter(fields), [](const auto& column) {
    return googlesql::StructField(column.name, column.type);
  });
  const googlesql::StructType* type = nullptr;
  if (auto status = type_factory.MakeStructType(fields, &type); !status.ok()) {
    return status;
  }
  return googlesql::TVFRelation::Column(name, type);
}

// Validates a call's literal arguments, which the translator reads, and gives its output:
// the query row for batch searches, the base row and their distance. The messages are BigQuery's,
// observed in tests/e2e/goclient/testdata/scalars/vector_search.txt.
absl::StatusOr<std::shared_ptr<googlesql::TVFSignature>> SearchSchema(
    googlesql::Catalog* /*catalog*/, googlesql::TypeFactory* type_factory,
    const googlesql::FunctionSignature& signature,
    const std::vector<googlesql::TVFInputArgumentType>& arguments,
    const googlesql::AnalyzerOptions& /*options*/) {
  const int64_t form = signature.context_id();
  const auto literal = [&](absl::string_view name) {
    return Literal(Argument(signature, arguments, name));
  };
  // These arguments must be literals, and only top_k can be NULL, for its default.
  const auto required = [&](absl::string_view name) -> absl::StatusOr<googlesql::Value> {
    auto value = literal(name);
    if (!value || (value->is_null() && name != "top_k")) {
      return Unsupported(absl::StrCat("with a non-literal or NULL ", name));
    }
    return *std::move(value);
  };
  const auto column_value = required("column_to_search");
  const auto top_k = required("top_k");
  const auto distance_value = required("distance_type");
  const auto options = required("options");
  for (const auto* value : {&column_value, &top_k, &distance_value, &options}) {
    if (!value->ok()) {
      return value->status();
    }
  }
  const auto lexical_columns = literal("lexical_search_columns");
  const auto lexical_query = literal("lexical_search_query_value");
  if (form == kBatch && (!lexical_columns || !lexical_columns->is_null())) {
    return absl::InvalidArgumentError(
        "lexical_search_columns is not supported when query_value is not specified.");
  }
  if (form == kBatch && (!lexical_query || !lexical_query->is_null())) {
    return absl::InvalidArgumentError(
        "lexical_search_query_value is not supported when query_value is not specified.");
  }
  if (!lexical_columns || !lexical_columns->is_null() || !lexical_query ||
      !lexical_query->is_null()) {
    return Unsupported("hybrid search");
  }
  const std::string column_to_search = column_value->string_value();
  if (auto status = CheckBaseColumn(arguments.at(0).relation(), column_to_search); !status.ok()) {
    return status;
  }
  const std::string distance_type = absl::AsciiStrToUpper(distance_value->string_value());
  if (distance_type != "EUCLIDEAN" && distance_type != "COSINE" && distance_type != "DOT_PRODUCT") {
    return absl::InvalidArgumentError(
        absl::StrCat("distance_type argument of VECTOR_SEARCH should be one of: EUCLIDEAN, COSINE, "
                     "DOT_PRODUCT. Got ",
                     distance_value->string_value(), "."));
  }
  if (auto status = CheckOptions(options->string_value()); !status.ok()) {
    return status;
  }
  googlesql::TVFRelation::ColumnList columns;
  if (form == kBatch) {
    const auto named = literal("query_column_to_search");
    if (!named) {
      return Unsupported("with a non-literal query_column_to_search");
    }
    const auto& query = arguments.at(2).relation();
    if (auto status = CheckQueryColumn(query, *named, column_to_search); !status.ok()) {
      return status;
    }
    auto column = StructColumn(*type_factory, query, "query");
    if (!column.ok()) {
      return column.status();
    }
    columns.push_back(*std::move(column));
  } else {
    const auto* argument = Argument(signature, arguments, "query_value");
    const auto scalar = argument->GetScalarArgType();
    if (!scalar.ok()) {
      return scalar.status();
    }
    const googlesql::Type* type = scalar->type();
    if (scalar->is_literal_null() || scalar->is_untyped_null()) {
      return absl::InvalidArgumentError(
          "One of the query data subquery or query_value of VECTOR_SEARCH must be specified.");
    }
    if (type->IsString() || type->IsStruct()) {
      return Unsupported("with autonomous embedding generation");
    }
    if (!IsEmbedding(type)) {
      return absl::InvalidArgumentError("query_value must be of type ARRAY<FLOAT64>");
    }
  }
  auto base = StructColumn(*type_factory, arguments.at(0).relation(), "base");
  if (!base.ok()) {
    return base.status();
  }
  columns.push_back(*std::move(base));
  columns.emplace_back("distance", googlesql::types::DoubleType());
  return googlesql::TVFSignature::Create(arguments, googlesql::TVFRelation(std::move(columns)));
}

}  // namespace

void AddVectorSearch(googlesql::SimpleCatalog& catalog) {
  catalog.AddOwnedTableValuedFunction(std::make_unique<googlesql::TableValuedFunction>(
      std::vector<std::string>{"vector_search"}, kBigQueryGroup,
      std::vector<googlesql::FunctionSignature>{Signature(kBatch), Signature(kSingle)},
      googlesql::TableValuedFunctionOptions().set_compute_result_type_callback(SearchSchema)));
}

}  // namespace bigquery_emulator_duckdb::analyzer_functions
