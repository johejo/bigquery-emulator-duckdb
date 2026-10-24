#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "src/zetasketch/hyperloglog_plus_plus.h"

namespace googlesql_hll {

// BigQuery's sketch value type identifiers.
enum class ValueType : int32_t {
  kInt64 = 8,
  kBytes = 11,
  kNumeric = 188553293,
  kBigNumeric = 188553294,
};
using Sketch = std::optional<zetasketch::HyperLogLogPlusPlus>;
uint64_t HashInt64(int64_t value);
uint64_t HashBytes(std::string_view value);
// NUMERIC's two's complement units, low word first.
uint64_t HashNumeric(uint64_t lower, int64_t upper);
// BIGNUMERIC's 32-byte, big-endian two's complement units.
uint64_t HashBigNumeric(std::string bytes);
absl::StatusOr<zetasketch::HyperLogLogPlusPlus*> Initialize(Sketch& sketch, ValueType type,
                                                            int64_t precision);
const char* MergeInto(Sketch& target, const zetasketch::HyperLogLogPlusPlus& source);
const char* MergeSerialized(Sketch& target, std::string_view bytes);
absl::StatusOr<int64_t> Extract(std::optional<std::string_view> bytes);

}  // namespace googlesql_hll
