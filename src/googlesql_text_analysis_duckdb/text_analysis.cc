#include "src/googlesql_text_analysis_duckdb/text_analysis.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/value.h"
#include "src/backend_error.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_functions_duckdb/any_arguments.h"
#include "src/googlesql_text_analysis/search.h"
#include "src/googlesql_text_analysis/text_analysis.h"

namespace bigquery_emulator_duckdb::text_analysis_duckdb {
namespace {
using namespace duckdb_functions;
using googlesql_text_analysis::Analyzer;
using googlesql_text_analysis::Dictionary;
using googlesql_text_analysis::DocumentCounts;
using googlesql_text_analysis::Score;

struct Scores {
  std::vector<Score> values;
};

// TEXT_ANALYZE(text, analyzer, analyzer_options). The analyzer is parsed again only when its
// arguments change from one row to the next.
void TextAnalyze(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  std::optional<std::pair<std::string, std::string>> key;
  absl::StatusOr<std::shared_ptr<const Analyzer>> analyzer;
  EachRow(info, input, output,
          [&](const Arguments& arguments) -> absl::StatusOr<std::vector<std::string>> {
            std::pair<std::string, std::string> arguments_key(arguments.String(1),
                                                              arguments.String(2));
            if (key != arguments_key) {
              analyzer = googlesql_text_analysis::ParseAnalyzer(
                  arguments_key.first, arguments_key.second, "TEXT_ANALYZE");
              key = std::move(arguments_key);
            }
            if (!analyzer.ok()) {
              return analyzer.status();
            }
            return googlesql_text_analysis::Analyze(**analyzer, arguments.String(0));
          });
}

struct ParsedSearch {
  std::shared_ptr<const googlesql_text_analysis::SearchQuery> query;
  googlesql_text_analysis::JsonScope scope = googlesql_text_analysis::JsonScope::kValues;
};

// SEARCH's search_query, json_scope, analyzer and analyzer_options, where nullopt is NULL.
using SearchArguments = std::array<std::optional<std::string>, 4>;

absl::StatusOr<ParsedSearch> ParseSearch(const SearchArguments& arguments) {
  constexpr std::array<std::string_view, 3> kNames = {
      "json_scope",
      "analyzer",
      "analyzer_options",
  };
  std::array<std::string_view, 4> values;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::optional<std::string>& argument = arguments.at(i);
    if (!argument) {
      return absl::OutOfRangeError(
          i == 0 ? "The 2nd argument of SEARCH() must not be null"
                 : absl::StrCat("'", kNames.at(i - 1), "' of SEARCH() must not be null"));
    }
    values.at(i) = *argument;
  }
  ParsedSearch parsed;
  GOOGLESQL_ASSIGN_OR_RETURN(parsed.scope, googlesql_text_analysis::ParseJsonScope(values.at(1)));
  GOOGLESQL_ASSIGN_OR_RETURN(
      auto analyzer, googlesql_text_analysis::ParseAnalyzer(values.at(2), values.at(3), ""));
  GOOGLESQL_ASSIGN_OR_RETURN(
      parsed.query, googlesql_text_analysis::ParseSearchQuery(values.at(0), std::move(analyzer)));
  return parsed;
}

// SEARCH(data_to_search, search_query, json_scope, analyzer, analyzer_options), whose data comes
// paired with its STRUCT names. The query is parsed again only when its arguments change from one
// row to the next.
void Search(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const functions_duckdb::AnyArguments values(input, {0});
  std::optional<SearchArguments> key;
  absl::StatusOr<ParsedSearch> parsed;
  EachRow(
      info, input, output,
      [&](const Arguments& arguments) -> absl::StatusOr<std::optional<bool>> {
        SearchArguments arguments_key;
        for (idx_t column = 1; column < 5; ++column) {
          if (!arguments.IsNull(column)) {
            arguments_key.at(column - 1) = arguments.String(column);
          }
        }
        if (key != arguments_key) {
          parsed = ParseSearch(arguments_key);
          key = std::move(arguments_key);
        }
        if (!parsed.ok()) {
          return parsed.status();
        }
        GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::Value data, values.Get(0, arguments.Row()));
        return googlesql_text_analysis::Search(data, *parsed->query, parsed->scope);
      },
      /*nulls=*/false);
}

// The error of SEARCH's constant arguments, which BigQuery reports even without rows to search.
struct SearchBindData {
  std::string error;
};

void* CopySearchBindData(void* data) {
  return new SearchBindData(*static_cast<const SearchBindData*>(data));
}

void DestroySearchBindData(void* data) { delete static_cast<SearchBindData*>(data); }

// The value of a constant argument, or nullopt when it is not constant.
std::optional<std::optional<std::string>> Constant(duckdb_bind_info info,
                                                   duckdb_client_context context, idx_t index) {
  Handle<duckdb_expression, duckdb_destroy_expression> const argument(
      duckdb_scalar_function_bind_get_argument(info, index));
  if (!duckdb_expression_is_foldable(argument.get())) {
    return std::nullopt;
  }
  duckdb_value folded = nullptr;
  Handle<duckdb_error_data, duckdb_destroy_error_data> const error(
      duckdb_expression_fold(context, argument.get(), &folded));
  Handle<duckdb_value, duckdb_destroy_value> const value(folded);
  if (duckdb_error_data_has_error(error.get())) {
    return std::nullopt;
  }
  if (duckdb_is_null_value(value.get())) {
    return std::optional<std::string>();
  }
  char* text = duckdb_get_varchar(value.get());
  std::string result(text);
  duckdb_free(text);
  return std::optional<std::string>(std::move(result));
}

void BindSearch(duckdb_bind_info info) {
  duckdb_client_context context_handle = nullptr;
  duckdb_scalar_function_get_client_context(info, &context_handle);
  Handle<duckdb_client_context, duckdb_destroy_client_context> const context(context_handle);
  SearchArguments arguments;
  for (idx_t index = 1; index < 5; ++index) {
    auto constant = Constant(info, context.get(), index);
    if (!constant) {
      return;
    }
    arguments.at(index - 1) = *std::move(constant);
  }
  if (const auto parsed = ParseSearch(arguments); !parsed.ok()) {
    duckdb_scalar_function_set_bind_data(
        info, new SearchBindData{std::string(parsed.status().message())}, DestroySearchBindData);
    duckdb_scalar_function_set_bind_data_copy(info, CopySearchBindData);
  }
}

// Raises the error of the constant arguments as each execution starts, where DuckDB reports it
// as it reports an execution error.
void InitSearch(duckdb_init_info info) {
  const auto* data =
      static_cast<const SearchBindData*>(duckdb_scalar_function_init_get_bind_data(info));
  if (data != nullptr) {
    duckdb_scalar_function_init_set_error(info, data->error.c_str());
  }
}

void RegisterSearch(duckdb_connection connection) {
  Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> const scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), "bq_search");
  LogicalType const any(duckdb_create_logical_type(kAny));
  LogicalType const varchar(duckdb_create_logical_type(kVarchar));
  duckdb_scalar_function_add_parameter(scalar.get(), any.get());
  for (int i = 0; i < 4; ++i) {
    duckdb_scalar_function_add_parameter(scalar.get(), varchar.get());
  }
  LogicalType const boolean(duckdb_create_logical_type(kBoolean));
  duckdb_scalar_function_set_return_type(scalar.get(), boolean.get());
  duckdb_scalar_function_set_function(scalar.get(), Search);
  duckdb_scalar_function_set_bind(scalar.get(), BindSearch);
  duckdb_scalar_function_set_init(scalar.get(), InitSearch);
  duckdb_scalar_function_set_special_handling(scalar.get());
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_search");
  }
}

// The terms of a document, where NULL is an element without one.
std::vector<std::optional<std::string>> Terms(const Arguments& arguments, idx_t column) {
  duckdb_vector vector = arguments.Vector(column);
  const auto entry = VectorElement<duckdb_list_entry>(vector, arguments.Row());
  duckdb_vector child = duckdb_list_vector_get_child(vector);
  uint64_t* validity = duckdb_vector_get_validity(child);
  std::vector<std::optional<std::string>> terms;
  terms.reserve(entry.length);
  for (idx_t i = entry.offset; i < entry.offset + entry.length; ++i) {
    if (validity == nullptr || duckdb_validity_row_is_valid(validity, i)) {
      terms.emplace_back(VectorString(child, i));
    } else {
      terms.emplace_back(std::nullopt);
    }
  }
  return terms;
}

DocumentCounts*& Counts(duckdb_aggregate_state state) {
  return *reinterpret_cast<DocumentCounts**>(state);
}

idx_t CountsSize(duckdb_function_info /*info*/) { return sizeof(DocumentCounts*); }

void InitCounts(duckdb_function_info /*info*/, duckdb_aggregate_state state) {
  Counts(state) = new DocumentCounts();
}

void DestroyCounts(duckdb_aggregate_state* states, idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    delete Counts(states[i]);
  }
}

// Counts each document, including NULL documents.
void CountDocuments(duckdb_function_info info, duckdb_data_chunk input,
                    duckdb_aggregate_state* states) {
  for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
    const Arguments arguments(input, row);
    const auto status = googlesql_text_analysis::AddDocument(
        *Counts(states[row]),
        arguments.IsNull(0) ? std::vector<std::optional<std::string>>() : Terms(arguments, 0),
        arguments.IsNull(1) ? std::nullopt : std::optional(arguments.Int(1)),
        arguments.IsNull(2) ? std::nullopt : std::optional(arguments.Int(2)));
    if (!status.ok()) {
      duckdb_aggregate_function_set_error(info, std::string(status.message()).c_str());
      return;
    }
  }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): DuckDB's callback.
void CombineCounts(duckdb_function_info /*info*/, duckdb_aggregate_state* source,
                   duckdb_aggregate_state* target, idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    googlesql_text_analysis::MergeCounts(*Counts(target[i]), *Counts(source[i]));
  }
}

// The private window dictionary encoding: unknown idf, then each term's idf, length and bytes.
std::string EncodeDictionary(const Dictionary& dictionary) {
  std::string blob;
  const auto append = [&blob](const auto& value) {
    blob.append(reinterpret_cast<const char*>(&value), sizeof(value));
  };
  append(dictionary.unknown);
  for (const auto& [term, idf] : dictionary.terms) {
    append(idf);
    append(term.size());
    blob += term;
  }
  return blob;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): DuckDB's callback.
void FinalizeCounts(duckdb_function_info /*info*/, duckdb_aggregate_state* source,
                    duckdb_vector result, idx_t count, idx_t offset) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  for (idx_t i = 0; i < count; ++i) {
    SetResult(result, offset + i,
              EncodeDictionary(googlesql_text_analysis::MakeDictionary(*Counts(source[i]))));
  }
}

Dictionary ParseDictionary(std::string_view blob) {
  const auto read = [&blob]<typename T>(T& value) {
    blob.copy(reinterpret_cast<char*>(&value), sizeof(value));
    blob.remove_prefix(sizeof(value));
  };
  Dictionary dictionary;
  read(dictionary.unknown);
  while (!blob.empty()) {
    double idf = 0;
    std::size_t size = 0;
    read(idf);
    read(size);
    dictionary.terms.emplace_back(blob.substr(0, size), idf);
    blob.remove_prefix(size);
  }
  return dictionary;
}

// Own the DuckDB list/STRUCT representation while the library returns typed scores.
void SetResult(duckdb_vector output, idx_t row, const Scores& scores) {
  const idx_t offset = duckdb_list_vector_get_size(output);
  duckdb_list_vector_reserve(output, offset + scores.values.size());
  duckdb_vector child = duckdb_list_vector_get_child(output);
  duckdb_vector term = duckdb_struct_vector_get_child(child, 0);
  duckdb_vector_ensure_validity_writable(child);
  duckdb_vector_ensure_validity_writable(term);
  for (idx_t i = 0; i < scores.values.size(); ++i) {
    const Score& score = scores.values.at(i);
    duckdb_functions::SetResult(term, offset + i, score.term);
    duckdb_functions::SetResult(duckdb_struct_vector_get_child(child, 1), offset + i, score.tf_idf);
  }
  duckdb_list_vector_set_size(output, offset + scores.values.size());
  static_cast<duckdb_list_entry*>(duckdb_vector_get_data(output))[row] = {
      .offset = offset,
      .length = scores.values.size(),
  };
}

// The scores of the document in argument 0 over the dictionary in argument 1: the unknown term's
// first, if the document has NULLs or terms outside the dictionary, then each term's in byte
// order. A term's tf is its count over the document's length, NULLs included. A NULL document has
// no scores. The dictionary is parsed again only when it changes from one row to the next.
void TfIdf(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  std::string blob;
  // Where the last dictionary's bytes were, which the window's rows share unless it is inlined.
  const char* last = nullptr;
  Dictionary dictionary;
  EachRow(
      info, input, output,
      [&](const Arguments& arguments) -> absl::StatusOr<Scores> {
        auto value = VectorElement<duckdb_string_t>(arguments.Vector(1), arguments.Row());
        const std::string_view current(duckdb_string_t_data(&value), duckdb_string_t_length(value));
        if (duckdb_string_is_inlined(value) || current.data() != last) {
          if (current != blob) {
            blob = current;
            dictionary = ParseDictionary(blob);
          }
          last = duckdb_string_is_inlined(value) ? nullptr : current.data();
        }
        if (arguments.IsNull(0)) {
          return Scores{};
        }
        return Scores{googlesql_text_analysis::TfIdf(Terms(arguments, 0), dictionary)};
      },
      /*nulls=*/false);
}

}  // namespace

void RegisterTextAnalysisFunctions(duckdb_connection connection) {
  LogicalType const varchar(duckdb_create_logical_type(kVarchar));
  LogicalType const list(duckdb_create_list_type(varchar.get()));
  Register(connection, "bq_text_analyze", {kVarchar, kVarchar, kVarchar}, list.get(), TextAnalyze);
  RegisterSearch(connection);

  LogicalType const bigint(duckdb_create_logical_type(kBigint));
  LogicalType const blob(duckdb_create_logical_type(kBlob));
  Handle<duckdb_aggregate_function, duckdb_destroy_aggregate_function> const dictionary(
      duckdb_create_aggregate_function());
  duckdb_aggregate_function_set_name(dictionary.get(), "bq_tf_idf_dictionary");
  duckdb_aggregate_function_add_parameter(dictionary.get(), list.get());
  duckdb_aggregate_function_add_parameter(dictionary.get(), bigint.get());
  duckdb_aggregate_function_add_parameter(dictionary.get(), bigint.get());
  duckdb_aggregate_function_set_return_type(dictionary.get(), blob.get());
  duckdb_aggregate_function_set_functions(dictionary.get(), CountsSize, InitCounts, CountDocuments,
                                          CombineCounts, FinalizeCounts);
  duckdb_aggregate_function_set_destructor(dictionary.get(), DestroyCounts);
  // NULL documents count toward the number of documents.
  duckdb_aggregate_function_set_special_handling(dictionary.get());
  if (duckdb_register_aggregate_function(connection, dictionary.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_tf_idf_dictionary");
  }

  LogicalType const dbl(duckdb_create_logical_type(kDouble));
  std::array<duckdb_logical_type, 2> fields = {varchar.get(), dbl.get()};
  std::array<const char*, 2> names = {"term", "tf_idf"};
  LogicalType const score(duckdb_create_struct_type(fields.data(), names.data(), fields.size()));
  LogicalType const scores(duckdb_create_list_type(score.get()));
  Register(connection, "bq_tf_idf", std::vector<duckdb_logical_type>{list.get(), blob.get()},
           scores.get(), TfIdf,
           /*nulls=*/false);
}

}  // namespace bigquery_emulator_duckdb::text_analysis_duckdb
