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
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/analyzer_functions/declarations.h"
#include "src/googlesql_functions/vector_search_options.h"
#include "src/language_options.h"

namespace bigquery_emulator_duckdb::analyzer_functions {
namespace {

// The batch and single forms' signatures.
constexpr int64_t kBatch = 0;
constexpr int64_t kSingle = 1;
constexpr absl::string_view kValueTableColumn = "$col1";

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

bool IsDistanceType(absl::string_view name) {
  const std::string upper = absl::AsciiStrToUpper(name);
  return upper == "EUCLIDEAN" || upper == "COSINE" || upper == "DOT_PRODUCT";
}

// Query data as BigQuery searches it: a value table is one column, named $col1.
googlesql::TVFRelation QueryRelation(const googlesql::TVFRelation& relation) {
  if (!relation.is_value_table()) {
    return relation;
  }
  return googlesql::TVFRelation({{kValueTableColumn, relation.column(0).type}});
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
// generation, which no emulator table has.
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
    return absl::InvalidArgumentError(absl::StrCat(
        "No generation expression found for the column_to_search: ", column.name, "."));
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

absl::StatusOr<googlesql::TVFRelation::Column> StructColumn(googlesql::TypeFactory& type_factory,
                                                            const googlesql::TVFRelation& relation,
                                                            const std::string& name) {
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
  // These arguments are literals or query parameters, whose values the catalog does not have,
  // so the translator checks them; nullopt stands for a parameter. A NULL is the default, but
  // column_to_search has none.
  const auto literal_or_parameter =
      [&](absl::string_view name) -> absl::StatusOr<std::optional<googlesql::Value>> {
    auto value = literal(name);
    if (value && value->is_null() && name == "column_to_search") {
      return absl::InvalidArgumentError("2nd argument of VECTOR_SEARCH must be specified.");
    }
    if (value) {
      return value;
    }
    // GoogleSQL gives a TVF a scalar argument's expression rather than its InputArgumentType's
    // parameter flag.
    if (const auto* expr = Argument(signature, arguments, name)->scalar_expr();
        expr != nullptr && expr->Is<googlesql::ResolvedParameter>()) {
      return std::nullopt;
    }
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid ", name,
                     " argument of VECTOR_SEARCH, should be a literal value or query parameter"));
  };
  const auto column_value = literal_or_parameter("column_to_search");
  const auto top_k = literal_or_parameter("top_k");
  const auto distance_value = literal_or_parameter("distance_type");
  const auto options = literal_or_parameter("options");
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
  // The column name, or nullopt for a parameter.
  const std::optional<std::string> column_to_search =
      *column_value ? std::optional((*column_value)->string_value()) : std::nullopt;
  // A base table query that is a value table selects an expression or renames its column, which
  // the analyzer rejects after resolving it.
  const auto& base_relation = arguments.at(0).relation();
  if (!base_relation.is_value_table() && column_to_search) {
    if (auto status = CheckBaseColumn(base_relation, *column_to_search); !status.ok()) {
      return status;
    }
  }
  if (const auto& distance = *distance_value; distance.has_value() && !distance->is_null()) {
    const std::string& distance_type = distance->string_value();
    if (!IsDistanceType(distance_type)) {
      return absl::InvalidArgumentError(absl::StrCat(
          "distance_type argument of VECTOR_SEARCH should be one of: EUCLIDEAN, COSINE, "
          "DOT_PRODUCT. Got ",
          distance_type, "."));
    }
  }
  if (const auto& text = *options; text.has_value() && !text->is_null()) {
    if (auto status = googlesql_functions::CheckVectorSearchOptions(text->string_value());
        !status.ok()) {
      return status;
    }
  }
  googlesql::TVFRelation::ColumnList columns;
  if (form == kBatch) {
    const auto named = literal_or_parameter("query_column_to_search");
    if (!named.ok()) {
      return named.status();
    }
    const auto query = QueryRelation(arguments.at(2).relation());
    // The default query column depends on column_to_search.
    if (const auto& name = *named;
        name.has_value() && (!name->is_null() || column_to_search.has_value())) {
      if (auto status = CheckQueryColumn(query, *name, column_to_search.value_or(""));
          !status.ok()) {
        return status;
      }
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
    if (!IsEmbedding(type)) {
      return absl::InvalidArgumentError("query_value must be of type ARRAY<FLOAT64>");
    }
  }
  if (base_relation.is_value_table()) {
    columns.emplace_back("base", base_relation.column(0).type);
  } else {
    auto base = StructColumn(*type_factory, base_relation, "base");
    if (!base.ok()) {
      return base.status();
    }
    columns.push_back(*std::move(base));
  }
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
