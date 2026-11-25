#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bigquery_emulator_duckdb {

struct KeyBound {
  std::string key;
  bool inclusive = true;
};

// The keys from `start` to `end`, unbounded where one is absent, ordered as unsigned bytes, as
// Bigtable orders row keys and BigQuery orders BYTES and valid UTF-8 STRING values.
struct KeyRange {
  std::optional<KeyBound> start;
  std::optional<KeyBound> end;
};

// A set of keys as ranges. Each function returns them sorted, disjoint and non-empty, and none
// of them for no keys.
using KeyRanges = std::vector<KeyRange>;

KeyRanges NormalizeKeyRanges(KeyRanges ranges);
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): intersection is commutative.
KeyRanges IntersectKeyRanges(const KeyRanges& left, const KeyRanges& right);
KeyRanges UnionKeyRanges(KeyRanges left, const KeyRanges& right);
bool KeyRangesContain(const KeyRanges& ranges, std::string_view key);
// The keys that start with `prefix`.
KeyRanges PrefixKeyRanges(const std::string& prefix);

}  // namespace bigquery_emulator_duckdb
