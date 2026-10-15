#include "src/backend_functions/text_analysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/functions/normalize_mode.pb.h"
#include "googlesql/public/functions/regexp.h"
#include "googlesql/public/functions/string.h"
#include "nlohmann/json.hpp"
#include "re2/re2.h"
#include "src/backend_error.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

namespace fn = googlesql::functions;

// LOG_ANALYZER's default delimiters as BigQuery documents them, where \s is a space.
constexpr auto kLogDelimiters = std::to_array<std::string_view>({
    "[",   "]",   "<",   ">",   "(",   ")",   "{",   "}",   "|",     "!",   ";",     ",",
    "'",   "\"",  "*",   "&",   "?",   "+",   "/",   ":",   "=",     "@",   ".",     "-",
    "$",   "%",   "\\",  "_",   "\n",  "\r",  " ",   "\t",  "%21",   "%26", "%2526", "%3B",
    "%3b", "%7C", "%7c", "%20", "%2B", "%2b", "%3D", "%3d", "%2520", "%5D", "%5d",   "%5B",
    "%5b", "%3A", "%3a", "%0A", "%0a", "%2C", "%2c", "%28", "%29",
});

// PATTERN_ANALYZER's pattern when analyzer_options gives none.
constexpr std::string_view kDefaultPattern = R"(\b\w{2,}\b)";

// BigQuery limits a delimiter to 16 characters.
constexpr std::size_t kMaxDelimiterLength = 16;

constexpr std::string_view kErrorSuffix = "; error in TEXT_ANALYZE expression";

struct Normalizer {
  enum class Mode : std::uint8_t { kNone, kLower, kUnicodeLower, kIcuNormalize };
  Mode mode = Mode::kLower;
  fn::NormalizeMode icu_normalize_mode = fn::NFKC;
  bool icu_case_folding = true;
};

constexpr auto kNormalizerModes = std::to_array<std::pair<std::string_view, Normalizer::Mode>>({
    {"NONE", Normalizer::Mode::kNone},
    {"LOWER", Normalizer::Mode::kLower},
    {"UNICODE_LOWER", Normalizer::Mode::kUnicodeLower},
    {"ICU_NORMALIZE", Normalizer::Mode::kIcuNormalize},
});

using StopWords = std::set<std::string, std::less<>>;
using TokenFilter = std::variant<Normalizer, StopWords>;

struct Analyzer {
  enum class Kind : std::uint8_t { kLog, kNoOp, kPattern };
  Kind kind = Kind::kLog;
  // The longest first, which takes precedence over a shorter one with the same prefix.
  std::vector<std::string> delimiters;
  std::unique_ptr<const fn::RegExp> pattern;
  std::vector<TokenFilter> filters;
};

absl::Status Failed(std::string_view message) {
  return absl::OutOfRangeError(absl::StrCat(message, kErrorSuffix));
}

// BigQuery checks analyzer_options against a JSON Schema and reports the first violation, at
// the JSON Pointer `pointer` to the value `instance`, as the JSON Schema validator for nlohmann
// JSON words it.
absl::Status SchemaError(std::string_view pointer, const nlohmann::json& instance,
                         std::string_view message) {
  return Failed(absl::StrCat("Invalid Argument Error: At ", pointer, " of ", instance.dump(), " - ",
                             message, "\n"));
}

std::size_t CodePoints(std::string_view text) {
  return std::ranges::count_if(
      text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; });
}

// What the schema allows of an array of strings.
struct StringsSchema {
  std::size_t min_items = 1;
  std::size_t max_items = std::numeric_limits<std::size_t>::max();
  std::size_t min_length = 0;
  std::size_t max_length = std::numeric_limits<std::size_t>::max();
};

absl::Status ValidateStrings(const std::string& pointer, const nlohmann::json& value,
                             const StringsSchema& schema) {
  if (!value.is_array()) {
    return SchemaError(pointer, value, "unexpected instance type");
  }
  if (value.size() > schema.max_items) {
    return SchemaError(pointer, value, "array has too many items");
  }
  if (value.size() < schema.min_items) {
    return SchemaError(pointer, value, "array has too few items");
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    const std::string element_pointer = absl::StrCat(pointer, "/", i);
    const nlohmann::json& element = value.at(i);
    if (!element.is_string()) {
      return SchemaError(element_pointer, element, "unexpected instance type");
    }
    const std::size_t length = CodePoints(element.get_ref<const std::string&>());
    if (length > schema.max_length) {
      return SchemaError(
          element_pointer, element,
          absl::StrCat("instance is too long as per maxLength: ", schema.max_length));
    }
    if (length < schema.min_length) {
      return SchemaError(
          element_pointer, element,
          absl::StrCat("instance is too short as per minLength:", schema.min_length));
    }
  }
  return absl::OkStatus();
}

std::vector<std::string> Strings(const nlohmann::json& value) {
  return value.get<std::vector<std::string>>();
}

// A token filter: one of a normalizer and stop words, or nothing when the filter matches neither.
std::optional<TokenFilter> ParseTokenFilter(const nlohmann::json& filter) {
  if (!filter.is_object() || filter.size() != 1) {
    return std::nullopt;
  }
  if (filter.contains("stop_words")) {
    const nlohmann::json& words = filter.at("stop_words");
    if (!ValidateStrings("", words, {.min_length = 1}).ok()) {
      return std::nullopt;
    }
    const std::vector<std::string> strings = Strings(words);
    return StopWords(strings.begin(), strings.end());
  }
  const auto normalizer_json = filter.find("normalizer");
  if (normalizer_json == filter.end() || !normalizer_json->is_object()) {
    return std::nullopt;
  }
  Normalizer normalizer;
  for (const auto& [key, option] : normalizer_json->items()) {
    bool valid = false;
    if (key == "mode" && option.is_string()) {
      const auto* const mode =
          std::ranges::find(kNormalizerModes, option.get<std::string>(),
                            &std::pair<std::string_view, Normalizer::Mode>::first);
      valid = mode != kNormalizerModes.end();
      normalizer.mode = valid ? mode->second : normalizer.mode;
    } else if (key == "icu_normalize_mode" && option.is_string()) {
      valid = fn::NormalizeMode_Parse(option.get<std::string>(), &normalizer.icu_normalize_mode);
    } else if (key == "icu_case_folding" && option.is_boolean()) {
      normalizer.icu_case_folding = option.get<bool>();
      valid = true;
    }
    if (!valid) {
      return std::nullopt;
    }
  }
  return normalizer;
}

absl::StatusOr<std::vector<TokenFilter>> TokenFilters(const nlohmann::json& value) {
  if (!value.is_array()) {
    return SchemaError("/token_filters", value, "unexpected instance type");
  }
  if (value.empty()) {
    return SchemaError("/token_filters", value, "array has too few items");
  }
  std::vector<TokenFilter> filters;
  for (std::size_t i = 0; i < value.size(); ++i) {
    std::optional<TokenFilter> filter = ParseTokenFilter(value.at(i));
    if (!filter) {
      return SchemaError(absl::StrCat("/token_filters/", i), value.at(i),
                         "no subschema has succeeded, but one of them is required to validate. "
                         "Type: oneOf, number of failed subschemas: 2");
    }
    filters.push_back(*std::move(filter));
  }
  return filters;
}

absl::StatusOr<std::unique_ptr<const fn::RegExp>> Pattern(const std::string& pattern) {
  auto regexp = fn::MakeRegExpUtf8(pattern);
  if (!regexp.ok()) {
    return absl::OutOfRangeError(absl::StrCat("Invalid regular expression pattern '", pattern,
                                              "'. Error in TEXT_ANALYZE expression"));
  }
  if (const int groups = (*regexp)->re().NumberOfCapturingGroups(); groups > 0) {
    return absl::OutOfRangeError(
        absl::StrCat("Capturing groups are not allowed for pattern analyzer. Found ", groups,
                     " in pattern '", pattern, "'. Error in TEXT_ANALYZE expression"));
  }
  return regexp;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): TEXT_ANALYZE's analyzer and options.
absl::StatusOr<Analyzer> ParseAnalyzer(std::string_view name, std::string_view options) {
  Analyzer analyzer;
  if (name == "LOG_ANALYZER") {
    analyzer.kind = Analyzer::Kind::kLog;
  } else if (name == "NO_OP_ANALYZER") {
    analyzer.kind = Analyzer::Kind::kNoOp;
  } else if (name == "PATTERN_ANALYZER") {
    analyzer.kind = Analyzer::Kind::kPattern;
  } else {
    return Failed(absl::StrCat("Invalid analyzer_name '", name, "'"));
  }
  const auto json = nlohmann::json::parse(options, nullptr, false);
  if (json.is_discarded()) {
    return Failed(
        absl::StrCat("Analyzer options must be a valid JSON-formatted string, got: ", options));
  }
  if (!json.is_object()) {
    return SchemaError("", json, "unexpected instance type");
  }
  std::optional<std::vector<std::string>> delimiters;
  std::string pattern(kDefaultPattern);
  analyzer.filters = {Normalizer{.mode = Normalizer::Mode::kLower}};
  for (const auto& [key, value] : json.items()) {
    if (key == "delimiters" && analyzer.kind == Analyzer::Kind::kLog) {
      GOOGLESQL_RETURN_IF_ERROR(ValidateStrings(
          "/delimiters", value, {.min_length = 1, .max_length = kMaxDelimiterLength}));
      delimiters = Strings(value);
    } else if (key == "patterns" && analyzer.kind == Analyzer::Kind::kPattern) {
      GOOGLESQL_RETURN_IF_ERROR(ValidateStrings("/patterns", value, {.max_items = 1}));
      pattern = value.at(0).get<std::string>();
    } else if (key == "token_filters" && analyzer.kind != Analyzer::Kind::kNoOp) {
      GOOGLESQL_ASSIGN_OR_RETURN(analyzer.filters, TokenFilters(value));
    } else {
      return SchemaError("", json,
                         absl::StrCat("validation failed for additional property '", key,
                                      "': instance invalid as per false-schema"));
    }
  }
  switch (analyzer.kind) {
    case Analyzer::Kind::kNoOp:
      analyzer.filters.clear();
      break;
    case Analyzer::Kind::kPattern: {
      GOOGLESQL_ASSIGN_OR_RETURN(analyzer.pattern, Pattern(pattern));
      break;
    }
    case Analyzer::Kind::kLog:
      if (!delimiters) {
        analyzer.delimiters.assign(kLogDelimiters.begin(), kLogDelimiters.end());
        break;
      }
      if (const auto non_ascii =
              std::ranges::find_if(*delimiters,
                                   [](const std::string& delimiter) {
                                     return !std::ranges::all_of(
                                         delimiter, [](char c) { return absl::ascii_isascii(c); });
                                   });
          non_ascii != delimiters->end()) {
        return Failed(
            absl::StrCat("delimiter ", *non_ascii,
                         " in custom LOG_ANALYZER config contains a non-ascii character"));
      }
      analyzer.delimiters = *std::move(delimiters);
      break;
  }
  std::ranges::stable_sort(analyzer.delimiters, std::ranges::greater{}, &std::string::size);
  return analyzer;
}

// The terms between the delimiters, without empty ones. Delimiters are ASCII, so they never
// match inside a multibyte character.
std::vector<std::string> SplitAtDelimiters(std::string_view text,
                                           const std::vector<std::string>& delimiters) {
  std::vector<std::string> terms;
  std::string term;
  for (std::size_t i = 0; i < text.size();) {
    const auto delimiter = std::ranges::find_if(
        delimiters, [&](const std::string& d) { return text.substr(i).starts_with(d); });
    if (delimiter == delimiters.end()) {
      term += text.at(i++);
      continue;
    }
    if (!term.empty()) {
      terms.push_back(std::move(term));
      term.clear();
    }
    i += delimiter->size();
  }
  if (!term.empty()) {
    terms.push_back(std::move(term));
  }
  return terms;
}

// The successive matches of the pattern, each searched for after the previous one, without
// empty ones.
std::vector<std::string> Matches(std::string_view text, const RE2& pattern) {
  std::vector<std::string> terms;
  absl::string_view match;
  std::size_t position = 0;
  while (position <= text.size() &&
         pattern.Match(text, position, text.size(), RE2::UNANCHORED, &match, 1)) {
    position = match.data() - text.data() + match.size();
    if (!match.empty()) {
      terms.emplace_back(match);
      continue;
    }
    // Step over the character after an empty match.
    ++position;
    while (position < text.size() &&
           (static_cast<unsigned char>(text.at(position)) & 0xC0U) == 0x80U) {
      ++position;
    }
  }
  return terms;
}

absl::StatusOr<std::string> Normalize(const Normalizer& normalizer, std::string term) {
  std::string out;
  absl::Status error;
  switch (normalizer.mode) {
    case Normalizer::Mode::kNone:
      return term;
    case Normalizer::Mode::kLower:
      absl::AsciiStrToLower(&term);
      return term;
    case Normalizer::Mode::kUnicodeLower:
      if (!fn::LowerUtf8(term, &out, &error)) {
        return error;
      }
      return out;
    case Normalizer::Mode::kIcuNormalize:
      if (!fn::Normalize(term, normalizer.icu_normalize_mode, normalizer.icu_case_folding, &out,
                         &error)) {
        return error;
      }
      return out;
  }
  return term;
}

absl::StatusOr<std::vector<std::string>> Analyze(const Analyzer& analyzer, std::string text) {
  std::vector<std::string> terms;
  switch (analyzer.kind) {
    case Analyzer::Kind::kNoOp:
      // An empty text has no term.
      if (!text.empty()) {
        terms.push_back(std::move(text));
      }
      return terms;
    case Analyzer::Kind::kLog:
      terms = SplitAtDelimiters(text, analyzer.delimiters);
      break;
    case Analyzer::Kind::kPattern:
      terms = Matches(text, analyzer.pattern->re());
      break;
  }
  for (const TokenFilter& filter : analyzer.filters) {
    if (const auto* words = std::get_if<StopWords>(&filter)) {
      std::erase_if(terms, [words](const std::string& term) { return words->contains(term); });
      continue;
    }
    for (std::string& term : terms) {
      GOOGLESQL_ASSIGN_OR_RETURN(term, Normalize(std::get<Normalizer>(filter), std::move(term)));
    }
  }
  return terms;
}

// TEXT_ANALYZE(text, analyzer, analyzer_options). The analyzer is parsed again only when its
// arguments change from one row to the next.
void TextAnalyze(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  std::optional<std::pair<std::string, std::string>> key;
  absl::StatusOr<Analyzer> analyzer;
  EachRow(info, input, output,
          [&](const Arguments& arguments) -> absl::StatusOr<std::vector<std::string>> {
            std::pair<std::string, std::string> arguments_key(arguments.String(1),
                                                              arguments.String(2));
            if (key != arguments_key) {
              analyzer = ParseAnalyzer(arguments_key.first, arguments_key.second);
              key = std::move(arguments_key);
            }
            if (!analyzer.ok()) {
              return analyzer.status();
            }
            return Analyze(*analyzer, arguments.String(0));
          });
}

// TF_IDF's largest max_distinct_tokens. The analyzer checks literal limits; CountDocuments checks
// query parameters with its messages.
constexpr int64_t kMaxDistinctTokens = 1048576;

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

// The documents of a TF_IDF window, NULL and empty ones included, how many of them contain each
// term, and the limits of the dictionary, which are constant.
struct DocumentCounts {
  int64_t documents = 0;
  std::unordered_map<std::string, int64_t> containing;
  int64_t max_distinct_tokens = 0;
  int64_t frequency_threshold = 0;
};

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

// Counts each row's document, argument 0, given max_distinct_tokens and frequency_threshold.
void CountDocuments(duckdb_function_info info, duckdb_data_chunk input,
                    duckdb_aggregate_state* states) {
  for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
    const Arguments arguments(input, row);
    for (const idx_t column : {1, 2}) {
      if (arguments.IsNull(column)) {
        duckdb_aggregate_function_set_error(
            info,
            absl::StrCat("Argument ", column + 1, " to BIGQUERYGOOGLESQL:TF_IDF must be non-NULL")
                .c_str());
        return;
      }
    }
    DocumentCounts& counts = *Counts(states[row]);
    counts.max_distinct_tokens = arguments.Int(1);
    counts.frequency_threshold = arguments.Int(2);
    if (counts.max_distinct_tokens < 1 || counts.max_distinct_tokens > kMaxDistinctTokens) {
      duckdb_aggregate_function_set_error(
          info, absl::StrCat("Argument 2 to BIGQUERYGOOGLESQL:TF_IDF must be between 1 and ",
                             kMaxDistinctTokens)
                    .c_str());
      return;
    }
    if (counts.frequency_threshold < 0) {
      duckdb_aggregate_function_set_error(
          info, "Argument 3 to BIGQUERYGOOGLESQL:TF_IDF must be at least 0");
      return;
    }
    ++counts.documents;
    if (arguments.IsNull(0)) {
      continue;
    }
    std::set<std::string> distinct;
    for (std::optional<std::string>& term : Terms(arguments, 0)) {
      if (term) {
        distinct.insert(*std::move(term));
      }
    }
    for (const std::string& term : distinct) {
      ++counts.containing[term];
    }
  }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): DuckDB's callback.
void CombineCounts(duckdb_function_info /*info*/, duckdb_aggregate_state* source,
                   duckdb_aggregate_state* target, idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    const DocumentCounts& from = *Counts(source[i]);
    DocumentCounts& to = *Counts(target[i]);
    to.documents += from.documents;
    for (const auto& [term, documents] : from.containing) {
      to.containing[term] += documents;
    }
    if (from.documents > 0) {
      to.max_distinct_tokens = from.max_distinct_tokens;
      to.frequency_threshold = from.frequency_threshold;
    }
  }
}

// The dictionary of a window as a BLOB: the unknown term's idf, then each term in byte order as
// its idf, its length and its bytes. A term's idf is ln(1 + documents / (1 + documents containing
// it)), and the unknown term's is the mean of the dictionary's, or 0 for an empty one.
std::string Dictionary(const DocumentCounts& counts) {
  std::vector<std::pair<std::string, int64_t>> terms;
  for (const auto& [term, documents] : counts.containing) {
    if (documents >= counts.frequency_threshold) {
      terms.emplace_back(term, documents);
    }
  }
  // The terms in the most documents make the dictionary, the first in byte order among equals.
  std::ranges::sort(terms, [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  if (std::cmp_greater(terms.size(), counts.max_distinct_tokens)) {
    terms.resize(counts.max_distinct_tokens);
  }
  std::ranges::sort(terms);
  std::vector<std::pair<std::string, double>> idfs;
  idfs.reserve(terms.size());
  double sum = 0;
  for (auto& [term, documents] : terms) {
    const double ratio = static_cast<double>(counts.documents) / static_cast<double>(1 + documents);
    // BigQuery's scores round as log(1 + x) does, which log1p(x) would not.
    // cppcheck-suppress unpreciseMathCall
    sum += idfs.emplace_back(std::move(term), std::log(1 + ratio)).second;
  }
  const double unknown = idfs.empty() ? 0 : sum / static_cast<double>(idfs.size());
  std::string blob;
  const auto append = [&blob](const auto& value) {
    blob.append(reinterpret_cast<const char*>(&value), sizeof(value));
  };
  append(unknown);
  for (const auto& [term, idf] : idfs) {
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
    SetResult(result, offset + i, Dictionary(*Counts(source[i])));
  }
}

// A dictionary that Dictionary wrote.
struct ParsedDictionary {
  double unknown = 0;
  // The terms in byte order and their idfs.
  std::vector<std::pair<std::string, double>> terms;
};

ParsedDictionary ParseDictionary(std::string_view blob) {
  const auto read = [&blob]<typename T>(T& value) {
    blob.copy(reinterpret_cast<char*>(&value), sizeof(value));
    blob.remove_prefix(sizeof(value));
  };
  ParsedDictionary dictionary;
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

// One element of TF_IDF's result, where a NULL term is the unknown term.
struct Score {
  std::optional<std::string> term;
  double tf_idf = 0;
};

void SetResult(duckdb_vector output, idx_t row, const Score& score) {
  duckdb_vector term = duckdb_struct_vector_get_child(output, 0);
  duckdb_vector_ensure_validity_writable(term);
  backend_functions::SetResult(term, row, score.term);
  backend_functions::SetResult(duckdb_struct_vector_get_child(output, 1), row, score.tf_idf);
}

// The scores of the document in argument 0 over the dictionary in argument 1: the unknown term's
// first, if the document has NULLs or terms outside the dictionary, then each term's in byte
// order. A term's tf is its count over the document's length, NULLs included. A NULL document has
// no scores. The dictionary is parsed again only when it changes from one row to the next.
void TfIdf(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  std::string blob;
  // Where the last dictionary's bytes were, which the window's rows share unless it is inlined.
  const char* last = nullptr;
  ParsedDictionary dictionary;
  EachRow(
      info, input, output,
      [&](const Arguments& arguments) -> absl::StatusOr<std::vector<Score>> {
        auto value = VectorElement<duckdb_string_t>(arguments.Vector(1), arguments.Row());
        const std::string_view current(duckdb_string_t_data(&value), duckdb_string_t_length(value));
        if (duckdb_string_is_inlined(value) || current.data() != last) {
          if (current != blob) {
            blob = current;
            dictionary = ParseDictionary(blob);
          }
          last = duckdb_string_is_inlined(value) ? nullptr : current.data();
        }
        std::vector<Score> scores;
        if (arguments.IsNull(0)) {
          return scores;
        }
        const std::vector<std::optional<std::string>> terms = Terms(arguments, 0);
        const auto size = static_cast<double>(terms.size());
        int64_t unknown = 0;
        std::map<std::string_view, std::pair<int64_t, double>> counts;
        for (const std::optional<std::string>& term : terms) {
          const auto known = term ? std::ranges::lower_bound(dictionary.terms, *term, {},
                                                             &std::pair<std::string, double>::first)
                                  : dictionary.terms.end();
          if (known == dictionary.terms.end() || known->first != *term) {
            ++unknown;
          } else {
            auto& [count, idf] = counts[known->first];
            ++count;
            idf = known->second;
          }
        }
        if (unknown > 0) {
          scores.push_back({
              .term = std::nullopt,
              .tf_idf = static_cast<double>(unknown) / size * dictionary.unknown,
          });
        }
        for (const auto& [term, count_and_idf] : counts) {
          const auto& [count, idf] = count_and_idf;
          scores.push_back(
              {.term = std::string(term), .tf_idf = static_cast<double>(count) / size * idf});
        }
        return scores;
      },
      /*nulls=*/false);
}

}  // namespace

void RegisterTextAnalysisFunctions(duckdb_connection connection) {
  LogicalType const varchar(duckdb_create_logical_type(kVarchar));
  LogicalType const list(duckdb_create_list_type(varchar.get()));
  Register(connection, "bq_text_analyze", {kVarchar, kVarchar, kVarchar}, list.get(), TextAnalyze);

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

}  // namespace bigquery_emulator_duckdb::backend_functions
