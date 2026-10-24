#include "src/googlesql_text_analysis/text_analysis.h"

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
#include "googlesql/base/status_macros.h"
#include "googlesql/public/functions/normalize_mode.pb.h"
#include "googlesql/public/functions/regexp.h"
#include "googlesql/public/functions/string.h"
#include "nlohmann/json.hpp"
#include "re2/re2.h"

namespace googlesql_text_analysis {
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

}  // namespace

struct Analyzer {
  enum class Kind : std::uint8_t { kLog, kNoOp, kPattern };
  Kind kind = Kind::kLog;
  // The longest first, which takes precedence over a shorter one with the same prefix.
  std::vector<std::string> delimiters;
  std::unique_ptr<const fn::RegExp> pattern;
  std::vector<TokenFilter> filters;
};

namespace {

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

}  // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): TEXT_ANALYZE's analyzer and options.
absl::StatusOr<std::shared_ptr<const Analyzer>> ParseAnalyzer(std::string_view name,
                                                              std::string_view options) {
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
  return std::make_shared<const Analyzer>(std::move(analyzer));
}

namespace {

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

}  // namespace

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

absl::Status AddDocument(DocumentCounts& counts, std::vector<std::optional<std::string>> terms,
                         std::optional<int64_t> max_distinct_tokens,
                         std::optional<int64_t> frequency_threshold) {
  constexpr int64_t kMaxDistinctTokens = 1048576;
  if (!max_distinct_tokens) {
    return absl::OutOfRangeError("Argument 2 to BIGQUERYGOOGLESQL:TF_IDF must be non-NULL");
  }
  if (!frequency_threshold) {
    return absl::OutOfRangeError("Argument 3 to BIGQUERYGOOGLESQL:TF_IDF must be non-NULL");
  }
  counts.max_distinct_tokens = *max_distinct_tokens;
  counts.frequency_threshold = *frequency_threshold;
  if (counts.max_distinct_tokens < 1 || counts.max_distinct_tokens > kMaxDistinctTokens) {
    return absl::OutOfRangeError(absl::StrCat(
        "Argument 2 to BIGQUERYGOOGLESQL:TF_IDF must be between 1 and ", kMaxDistinctTokens));
  }
  if (counts.frequency_threshold < 0) {
    return absl::OutOfRangeError("Argument 3 to BIGQUERYGOOGLESQL:TF_IDF must be at least 0");
  }
  ++counts.documents;
  std::set<std::string> distinct;
  for (auto& term : terms) {
    if (term) {
      distinct.insert(*std::move(term));
    }
  }
  for (const auto& term : distinct) {
    ++counts.containing[term];
  }
  return absl::OkStatus();
}

void MergeCounts(DocumentCounts& to, const DocumentCounts& from) {
  to.documents += from.documents;
  for (const auto& [term, documents] : from.containing) {
    to.containing[term] += documents;
  }
  if (from.documents > 0) {
    to.max_distinct_tokens = from.max_distinct_tokens;
    to.frequency_threshold = from.frequency_threshold;
  }
}

Dictionary MakeDictionary(const DocumentCounts& counts) {
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
  return {.unknown = unknown, .terms = std::move(idfs)};
}

std::vector<Score> TfIdf(const std::vector<std::optional<std::string>>& terms,
                         const Dictionary& dictionary) {
  std::vector<Score> scores;
  const auto size = static_cast<double>(terms.size());
  int64_t unknown = 0;
  std::map<std::string_view, std::pair<int64_t, double>> counts;
  for (const std::optional<std::string>& term : terms) {
    const auto known = term ? std::ranges::lower_bound(dictionary.terms, *term, {},
                                                       &std::pair<std::string, double>::first)
                            : dictionary.terms.end();
    if (!term || known == dictionary.terms.end() || known->first != *term) {
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
}

}  // namespace googlesql_text_analysis
