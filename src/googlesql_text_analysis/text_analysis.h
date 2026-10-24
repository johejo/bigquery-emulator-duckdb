#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace googlesql_text_analysis {

struct Analyzer;
absl::StatusOr<std::shared_ptr<const Analyzer>> ParseAnalyzer(std::string_view name,
                                                              std::string_view options);
absl::StatusOr<std::vector<std::string>> Analyze(const Analyzer& analyzer, std::string text);

// NULL and empty documents both count toward the window's document count.
struct DocumentCounts {
  int64_t documents = 0;
  std::unordered_map<std::string, int64_t> containing;
  int64_t max_distinct_tokens = 0;
  int64_t frequency_threshold = 0;
};
absl::Status AddDocument(DocumentCounts& counts, std::vector<std::optional<std::string>> terms,
                         std::optional<int64_t> max_distinct_tokens,
                         std::optional<int64_t> frequency_threshold);
void MergeCounts(DocumentCounts& to, const DocumentCounts& from);

struct Dictionary {
  double unknown = 0;
  // The terms in byte order and their idfs.
  std::vector<std::pair<std::string, double>> terms;
};
// A term's idf is ln(1 + documents / (1 + documents containing it)); the unknown
// term's idf is the mean of the dictionary's, or 0 for an empty dictionary.
Dictionary MakeDictionary(const DocumentCounts& counts);

// A NULL term represents the unknown term.
struct Score {
  std::optional<std::string> term;
  double tf_idf = 0;
};
// Scores put the unknown term first, then known terms in byte order. Term frequency
// includes NULL terms in the document length. An empty document has no scores.
std::vector<Score> TfIdf(const std::vector<std::optional<std::string>>& terms,
                         const Dictionary& dictionary);

}  // namespace googlesql_text_analysis
