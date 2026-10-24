#include "src/googlesql_hll/hll.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "src/zetasketch/fingerprint2011.h"
#include "src/zetasketch/hyperloglog_plus_plus.h"

namespace googlesql_hll {
namespace {
using zetasketch::HyperLogLogPlusPlus;
constexpr int kDefaultSparsePrecisionDelta = 5;
constexpr char kInvalidSketch[] = "Invalid or incompatible sketch in HLL_COUNT.MERGE_PARTIAL";

// The little-endian two's complement bytes of `value`, in `size` bytes.
template <typename Integer>
std::string LittleEndian(Integer value, std::size_t size) {
  auto bits = static_cast<std::make_unsigned_t<Integer>>(value);
  std::string bytes(size, '\0');
  for (char& byte : bytes) {
    byte = static_cast<char>(bits & 0xFFU);
    bits >>= 8U;
  }
  return bytes;
}

}  // namespace

uint64_t HashInt64(int64_t value) { return HashBytes(LittleEndian(value, 8)); }
uint64_t HashBytes(std::string_view value) { return zetasketch::Fingerprint2011(value); }
uint64_t HashNumeric(uint64_t lower, int64_t upper) {
  return HashBytes(LittleEndian(lower, 8) + LittleEndian(upper, 8));
}
uint64_t HashBigNumeric(std::string bytes) {
  std::ranges::reverse(bytes);
  return HashBytes(bytes);
}

absl::StatusOr<zetasketch::HyperLogLogPlusPlus*> Initialize(Sketch& sketch, ValueType type,
                                                            int64_t precision) {
  if (!sketch) {
    if (precision < 10 || precision > 24) {
      return absl::OutOfRangeError("Argument 2 to HLL_COUNT.INIT must be between 10 and 24");
    }
    const auto normal = static_cast<int>(precision);
    sketch.emplace(normal,
                   std::min(normal + kDefaultSparsePrecisionDelta,
                            HyperLogLogPlusPlus::kMaximumSparsePrecision),
                   static_cast<int32_t>(type));
  }
  return &*sketch;
}

// Merges `source` into `target`, or returns the error MERGE_PARTIAL fails with.
const char* MergeInto(Sketch& target, const HyperLogLogPlusPlus& source) {
  if (!target) {
    target = source;
    return nullptr;
  }
  switch (target->Merge(source)) {
    case HyperLogLogPlusPlus::MergeResult::kMerged:
      return nullptr;
    case HyperLogLogPlusPlus::MergeResult::kIncompatibleTypes:
      return "HLL_COUNT.MERGE_PARTIAL failed to compute.";
    case HyperLogLogPlusPlus::MergeResult::kIncompatiblePrecisions:
      return kInvalidSketch;
  }
  return nullptr;
}

const char* MergeSerialized(Sketch& target, std::string_view bytes) {
  const auto sketch = zetasketch::HyperLogLogPlusPlus::Parse(bytes);
  return sketch ? MergeInto(target, *sketch) : kInvalidSketch;
}

absl::StatusOr<int64_t> Extract(std::optional<std::string_view> bytes) {
  if (!bytes) {
    return 0;
  }
  auto sketch = zetasketch::HyperLogLogPlusPlus::Parse(*bytes);
  if (!sketch) {
    return absl::OutOfRangeError("Invalid input bytes to HLL_COUNT.EXTRACT");
  }
  return sketch->Estimate();
}

}  // namespace googlesql_hll
