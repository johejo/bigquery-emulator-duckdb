#include "src/key_ranges.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace bigquery_emulator_duckdb {
namespace {

// std::string compares its characters as unsigned char, which is the order of the keys.

// Whether `left` starts before `right`; an absent start is the lowest.
bool StartsBefore(const std::optional<KeyBound>& left, const std::optional<KeyBound>& right) {
  if (!left || !right) {
    return !left && right;
  }
  return left->key != right->key ? left->key < right->key : left->inclusive && !right->inclusive;
}

// Whether `left` ends before `right`; an absent end is the highest.
bool EndsBefore(const std::optional<KeyBound>& left, const std::optional<KeyBound>& right) {
  if (!left || !right) {
    return left && !right;
  }
  return left->key != right->key ? left->key < right->key : !left->inclusive && right->inclusive;
}

bool Empty(const KeyRange& range) {
  if (!range.start || !range.end) {
    return false;
  }
  return range.start->key != range.end->key ? range.end->key < range.start->key
                                            : !(range.start->inclusive && range.end->inclusive);
}

// Whether `next`, which does not start before `range`, overlaps or adjoins it.
bool Joins(const KeyRange& range, const KeyRange& next) {
  if (!range.end || !next.start) {
    return true;
  }
  return range.end->key != next.start->key ? next.start->key < range.end->key
                                           : range.end->inclusive || next.start->inclusive;
}

}  // namespace

KeyRanges NormalizeKeyRanges(KeyRanges ranges) {
  std::erase_if(ranges, Empty);
  std::ranges::sort(ranges, [](const KeyRange& left, const KeyRange& right) {
    return StartsBefore(left.start, right.start);
  });
  KeyRanges result;
  for (auto& range : ranges) {
    if (!result.empty() && Joins(result.back(), range)) {
      if (EndsBefore(result.back().end, range.end)) {
        result.back().end = std::move(range.end);
      }
    } else {
      result.push_back(std::move(range));
    }
  }
  return result;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): intersection is commutative.
KeyRanges IntersectKeyRanges(const KeyRanges& left, const KeyRanges& right) {
  KeyRanges result;
  for (const auto& a : left) {
    std::ranges::transform(right, std::back_inserter(result), [&a](const KeyRange& b) {
      return KeyRange{
          .start = StartsBefore(a.start, b.start) ? b.start : a.start,
          .end = EndsBefore(a.end, b.end) ? a.end : b.end,
      };
    });
  }
  return NormalizeKeyRanges(std::move(result));
}

KeyRanges UnionKeyRanges(KeyRanges left, const KeyRanges& right) {
  left.insert(left.end(), right.begin(), right.end());
  return NormalizeKeyRanges(std::move(left));
}

bool KeyRangesContain(const KeyRanges& ranges, std::string_view key) {
  return std::ranges::any_of(ranges, [key](const KeyRange& range) {
    const bool after_start =
        !range.start || (range.start->inclusive ? range.start->key <= key : range.start->key < key);
    const bool before_end =
        !range.end || (range.end->inclusive ? key <= range.end->key : key < range.end->key);
    return after_start && before_end;
  });
}

KeyRanges PrefixKeyRanges(const std::string& prefix) {
  // The prefix's successor ends the range: the shortest key greater than every key that starts
  // with it, absent when the prefix holds only 0xFF bytes.
  std::string end = prefix;
  while (!end.empty() && static_cast<unsigned char>(end.back()) == 0xFF) {
    end.pop_back();
  }
  KeyRange range{.start = KeyBound{.key = prefix, .inclusive = true}, .end = std::nullopt};
  if (!end.empty()) {
    end.back() = static_cast<char>(static_cast<unsigned char>(end.back()) + 1);
    range.end = KeyBound{.key = std::move(end), .inclusive = false};
  }
  return {std::move(range)};
}

}  // namespace bigquery_emulator_duckdb
