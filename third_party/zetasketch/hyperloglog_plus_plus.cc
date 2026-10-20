// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the
// License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
// express or implied. See the License for the specific language governing permissions and
// limitations under the License.
//
// Ported to C++ from zetasketch's HyperLogLogPlusPlus.java and, in
// java/com/google/zetasketch/internal/hllplus/, Encoding.java, NormalRepresentation.java,
// SparseRepresentation.java and State.java; see README.md.

#include "third_party/zetasketch/hyperloglog_plus_plus.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "third_party/zetasketch/data.h"

namespace zetasketch {
namespace {

// AggregatorType.HYPERLOGLOG_PLUS_UNIQUE, which is also the field of its state.
constexpr uint32_t kHyperLogLogPlusUnique = 112;
constexpr int32_t kEncodingVersion = 2;
constexpr int32_t kDefaultEncodingVersion = 1;

constexpr uint32_t kVarint = 0;
constexpr uint32_t kFixed64 = 1;
constexpr uint32_t kLengthDelimited = 2;
constexpr uint32_t kFixed32 = 5;

// The bits of a sparse value that hold a sparse rhoW.
constexpr int kRhoWBits = 6;
constexpr uint32_t kRhoWMask = (uint32_t{1} << static_cast<unsigned>(kRhoWBits)) - 1;

// The share of the normal representation's size that the sparse data may take, and that the
// buffer may take in elements.
constexpr float kMaximumSparseDataFraction = 0.75F;
constexpr float kMaximumBufferElementsFraction = 1 - kMaximumSparseDataFraction;

// Reads the protocol buffer wire format.
class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  [[nodiscard]] bool AtEnd() const { return bytes_.empty(); }

  std::optional<uint64_t> Varint() {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64 && !bytes_.empty(); shift += 7) {
      const auto byte = static_cast<uint8_t>(bytes_.front());
      bytes_.remove_prefix(1);
      value |= static_cast<uint64_t>(byte & 0x7FU) << shift;
      if ((byte & 0x80U) == 0) {
        return value;
      }
    }
    return std::nullopt;
  }

  std::optional<std::string_view> LengthDelimited() {
    const auto length = Varint();
    if (!length || *length > bytes_.size()) {
      return std::nullopt;
    }
    const std::string_view value = bytes_.substr(0, *length);
    bytes_.remove_prefix(*length);
    return value;
  }

  // Skips a field of `wire_type`, failing for groups and unknown wire types.
  bool Skip(uint32_t wire_type) {
    switch (wire_type) {
      case kVarint:
        return Varint().has_value();
      case kLengthDelimited:
        return LengthDelimited().has_value();
      case kFixed64:
      case kFixed32: {
        const std::size_t size = wire_type == kFixed64 ? 8 : 4;
        if (bytes_.size() < size) {
          return false;
        }
        bytes_.remove_prefix(size);
        return true;
      }
      default:
        return false;
    }
  }

 private:
  std::string_view bytes_;
};

void WriteVarint(std::string& out, uint64_t value) {
  while (value >= 0x80U) {
    out.push_back(static_cast<char>((value & 0x7FU) | 0x80U));
    value >>= 7U;
  }
  out.push_back(static_cast<char>(value));
}

std::size_t VarintSize(uint64_t value) {
  std::size_t size = 1;
  while (value >= 0x80U) {
    value >>= 7U;
    ++size;
  }
  return size;
}

void WriteTag(std::string& out, uint32_t field, uint32_t wire_type) {
  WriteVarint(out, (static_cast<uint64_t>(field) << 3U) | wire_type);
}

// An int32 or enum field, which negative values sign-extend to ten bytes.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a field and its value.
void WriteInt(std::string& out, uint32_t field, int64_t value) {
  WriteTag(out, field, kVarint);
  WriteVarint(out, static_cast<uint64_t>(value));
}

void WriteBytes(std::string& out, uint32_t field, std::string_view bytes) {
  WriteTag(out, field, kLengthDelimited);
  WriteVarint(out, bytes.size());
  out += bytes;
}

// rho(w) of the last `bits` bits of `value`: one more than their leading zeros, or bits + 1 for
// none set. Java's shift by 64 leaves the value as it is.
uint8_t RhoW(uint64_t value, int bits) {
  const uint64_t w = value << static_cast<unsigned>((64 - bits) % 64);
  return static_cast<uint8_t>(w == 0 ? bits + 1 : std::countl_zero(w) + 1);
}

// rho(w) at `target` precision of a bucket `index` at `source` precision with rhoW `rho_w`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): zetasketch's order.
uint8_t DowngradeRhoW(uint32_t index, uint8_t rho_w, int source, int target) {
  if (source == target) {
    return rho_w;
  }
  const uint32_t suffix = index << static_cast<unsigned>(32 - source + target);
  if (suffix == 0) {
    return static_cast<uint8_t>(rho_w + source - target);
  }
  return static_cast<uint8_t>(1 + std::countl_zero(suffix));
}

std::vector<uint32_t> MergeSorted(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  std::vector<uint32_t> merged;
  merged.reserve(a.size() + b.size());
  std::ranges::merge(a, b, std::back_inserter(merged));
  return merged;
}

std::size_t DifferenceEncodedSize(const std::vector<uint32_t>& values) {
  std::size_t size = 0;
  uint32_t last = 0;
  for (const uint32_t value : values) {
    size += VarintSize(value - last);
    last = value;
  }
  return size;
}

}  // namespace

// The sparse encoding: a value is the sparse index when its last sparse_precision - precision
// bits determine the normal rhoW, or else a flag, the normal index and the sparse rhoW.
struct HyperLogLogPlusPlus::Sparse {
  int precision;
  int sparse_precision;

  [[nodiscard]] uint32_t Flag() const {
    return 1U << static_cast<unsigned>(std::max(sparse_precision, precision + kRhoWBits));
  }
  [[nodiscard]] unsigned Shift() const {
    return static_cast<unsigned>(sparse_precision - precision);
  }

  [[nodiscard]] uint32_t Encode(uint64_t hash) const {
    const auto index = static_cast<uint32_t>(hash >> static_cast<unsigned>(64 - sparse_precision));
    return Encode(index, RhoW(hash, 64 - sparse_precision));
  }
  [[nodiscard]] uint32_t Encode(uint32_t sparse_index, uint8_t sparse_rho_w) const {
    if ((sparse_index & ((1U << Shift()) - 1)) != 0) {
      return sparse_index;
    }
    return Flag() | (sparse_index >> Shift()) << static_cast<unsigned>(kRhoWBits) | sparse_rho_w;
  }
  [[nodiscard]] bool RhoEncoded(uint32_t value) const { return (value & Flag()) != 0; }
  [[nodiscard]] uint32_t SparseIndex(uint32_t value) const {
    return RhoEncoded(value) ? ((value ^ Flag()) >> static_cast<unsigned>(kRhoWBits)) << Shift()
                             : value;
  }
  [[nodiscard]] uint8_t SparseRhoWIfPresent(uint32_t value) const {
    return RhoEncoded(value) ? static_cast<uint8_t>(value & kRhoWMask) : 0;
  }
  [[nodiscard]] uint32_t NormalIndex(uint32_t value) const {
    return RhoEncoded(value) ? (value ^ Flag()) >> static_cast<unsigned>(kRhoWBits)
                             : value >> Shift();
  }
  [[nodiscard]] uint8_t NormalRhoW(uint32_t value) const {
    return RhoEncoded(value) ? static_cast<uint8_t>((value & kRhoWMask) + Shift())
                             : RhoW(value, static_cast<int>(Shift()));
  }
  // The value in `target`, whose precisions are at most these.
  [[nodiscard]] uint32_t Downgrade(uint32_t value, const Sparse& target) const {
    const uint32_t index = SparseIndex(value);
    const uint8_t rho_w =
        DowngradeRhoW(index, SparseRhoWIfPresent(value), sparse_precision, target.sparse_precision);
    return target.Encode(index >> static_cast<unsigned>(sparse_precision - target.sparse_precision),
                         rho_w);
  }
  [[nodiscard]] bool IsLessThan(const Sparse& other) const {
    return precision < other.precision || sparse_precision < other.sparse_precision;
  }
  [[nodiscard]] bool IsCompatible(const Sparse& other) const {
    return (precision <= other.precision && sparse_precision <= other.sparse_precision) ||
           (precision >= other.precision && sparse_precision >= other.sparse_precision);
  }
  // The sorted `values` without duplicates, keeping the greatest rhoW of a sparse index.
  [[nodiscard]] std::vector<uint32_t> Dedupe(const std::vector<uint32_t>& values) const {
    std::vector<uint32_t> deduped;
    for (const uint32_t value : values) {
      if (!deduped.empty() && SparseIndex(deduped.back()) == SparseIndex(value)) {
        deduped.back() = value;
      } else {
        deduped.push_back(value);
      }
    }
    return deduped;
  }
};

HyperLogLogPlusPlus::Sparse HyperLogLogPlusPlus::Encoding() const {
  return {.precision = precision_, .sparse_precision = sparse_precision_};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): zetasketch's order.
HyperLogLogPlusPlus::HyperLogLogPlusPlus(int precision, int sparse_precision, int32_t value_type)
    : value_type_(value_type),
      precision_(precision),
      sparse_precision_(sparse_precision),
      normal_(sparse_precision == 0) {}

std::optional<HyperLogLogPlusPlus> HyperLogLogPlusPlus::Parse(std::string_view sketch) {
  HyperLogLogPlusPlus result;
  uint64_t type = kHyperLogLogPlusUnique;
  int32_t encoding_version = kDefaultEncodingVersion;
  std::optional<std::string_view> data;
  std::optional<std::string_view> sparse_data;
  Reader reader(sketch);
  while (!reader.AtEnd()) {
    const auto tag = reader.Varint();
    if (!tag) {
      return std::nullopt;
    }
    const auto field = *tag >> 3U;
    const auto wire_type = static_cast<uint32_t>(*tag & 7U);
    if (field == kHyperLogLogPlusUnique && wire_type == kLengthDelimited) {
      const auto state = reader.LengthDelimited();
      if (!state) {
        return std::nullopt;
      }
      Reader hll(*state);
      while (!hll.AtEnd()) {
        const auto hll_tag = hll.Varint();
        if (!hll_tag) {
          return std::nullopt;
        }
        const auto hll_field = *hll_tag >> 3U;
        const auto hll_wire_type = static_cast<uint32_t>(*hll_tag & 7U);
        if (hll_wire_type == kVarint && hll_field >= 2 && hll_field <= 4) {
          const auto value = hll.Varint();
          if (!value) {
            return std::nullopt;
          }
          int& field_value = hll_field == 2   ? result.sparse_size_
                             : hll_field == 3 ? result.precision_
                                              : result.sparse_precision_;
          field_value = static_cast<int32_t>(*value);
        } else if (hll_wire_type == kLengthDelimited && (hll_field == 5 || hll_field == 6)) {
          std::optional<std::string_view>& bytes = hll_field == 5 ? data : sparse_data;
          bytes = hll.LengthDelimited();
          if (!bytes) {
            return std::nullopt;
          }
        } else if (!hll.Skip(hll_wire_type)) {
          return std::nullopt;
        }
      }
      continue;
    }
    if (wire_type == kVarint && field >= 1 && field <= 4) {
      const auto value = reader.Varint();
      if (!value) {
        return std::nullopt;
      }
      switch (field) {
        case 1:
          type = *value;
          break;
        case 2:
          result.num_values_ = static_cast<int64_t>(*value);
          break;
        case 3:
          encoding_version = static_cast<int32_t>(*value);
          break;
        default:
          result.value_type_ = static_cast<int32_t>(*value);
          break;
      }
    } else if (!reader.Skip(wire_type)) {
      return std::nullopt;
    }
  }
  if (type != kHyperLogLogPlusUnique || encoding_version != kEncodingVersion ||
      result.precision_ < kMinimumPrecision || result.precision_ > kMaximumPrecision) {
    return std::nullopt;
  }
  const bool has_data = data && !data->empty();
  const bool has_sparse_data = sparse_data && !sparse_data->empty();
  if (has_sparse_data && result.sparse_precision_ == 0) {
    return std::nullopt;
  }
  result.normal_ = has_data || result.sparse_precision_ == 0;
  if (result.normal_) {
    if (has_data) {
      if (data->size() != std::size_t{1} << static_cast<unsigned>(result.precision_)) {
        return std::nullopt;
      }
      result.data_.assign(data->begin(), data->end());
    }
    return result;
  }
  if (result.sparse_precision_ < result.precision_ ||
      result.sparse_precision_ > kMaximumSparsePrecision) {
    return std::nullopt;
  }
  if (sparse_data) {
    result.has_sparse_data_ = true;
    result.sparse_data_bytes_ = sparse_data->size();
    const uint64_t limit =
        uint64_t{result.Encoding().Flag()} << 1U;
    Reader values(*sparse_data);
    uint64_t value = 0;
    while (!values.AtEnd()) {
      const auto difference = values.Varint();
      if (!difference || *difference >= limit || value + *difference >= limit) {
        return std::nullopt;
      }
      value += *difference;
      result.sparse_data_.push_back(static_cast<uint32_t>(value));
    }
  }
  return result;
}

void HyperLogLogPlusPlus::AddHash(uint64_t hash) {
  ++num_values_;
  if (normal_) {
    EnsureData();
    const auto index = static_cast<std::size_t>(hash >> static_cast<unsigned>(64 - precision_));
    uint8_t& rho_w = data_.at(index);
    rho_w = std::max(rho_w, RhoW(hash, 64 - precision_));
    return;
  }
  buffer_.insert(Encoding().Encode(hash));
  UpdateRepresentation();
}

HyperLogLogPlusPlus::MergeResult HyperLogLogPlusPlus::Merge(const HyperLogLogPlusPlus& other) {
  if (value_type_ != other.value_type_ && value_type_ != 0 && other.value_type_ != 0) {
    return MergeResult::kIncompatibleTypes;
  }
  if (value_type_ == 0) {
    value_type_ = other.value_type_;
  }
  MergeResult result = MergeResult::kMerged;
  if (!normal_ && !other.normal_) {
    result = AddSparseValues(other.Encoding(), other.SortedSparseValues());
  } else {
    if (!normal_) {
      Normalize();
    }
    if (other.normal_) {
      MaybeDowngradeNormal(other.Encoding());
      MergeNormalData(other.data_, other.precision_);
    } else {
      // The sparse data and buffer of `other` merge into the registers in any order.
      MaybeDowngradeNormal(other.Encoding());
      EnsureData();
      for (const uint32_t value : other.sparse_data_) {
        AddNormalSparseValue(other.Encoding(), value);
      }
      for (const uint32_t value : other.buffer_) {
        AddNormalSparseValue(other.Encoding(), value);
      }
    }
  }
  num_values_ += other.num_values_;
  return result;
}

std::string HyperLogLogPlusPlus::Serialize() {
  if (!normal_) {
    FlushBuffer();
    has_sparse_data_ = true;
    UpdateRepresentation();
  }
  std::string state;
  if (sparse_size_ != 0) {
    WriteInt(state, 2, sparse_size_);
  }
  if (precision_ != 0) {
    WriteInt(state, 3, precision_);
  }
  if (sparse_precision_ != 0) {
    WriteInt(state, 4, sparse_precision_);
  }
  if (!data_.empty()) {
    WriteBytes(state, 5,
               std::string_view(reinterpret_cast<const char*>(data_.data()), data_.size()));
  }
  if (has_sparse_data_) {
    std::string sparse_data;
    uint32_t last = 0;
    for (const uint32_t value : sparse_data_) {
      WriteVarint(sparse_data, value - last);
      last = value;
    }
    WriteBytes(state, 6, sparse_data);
  }
  std::string sketch;
  WriteInt(sketch, 1, kHyperLogLogPlusUnique);
  WriteInt(sketch, 2, num_values_);
  WriteInt(sketch, 3, kEncodingVersion);
  if (value_type_ != 0) {
    WriteInt(sketch, 4, value_type_);
  }
  WriteBytes(sketch, kHyperLogLogPlusUnique, state);
  return sketch;
}

int64_t HyperLogLogPlusPlus::Estimate() {
  if (!normal_) {
    FlushBuffer();
    // LinearCounting over the empty sparse buckets.
    const double buckets = std::ldexp(1.0, sparse_precision_);
    const double zeros = buckets - sparse_size_;
    return static_cast<int64_t>(std::floor((buckets * std::log(buckets / zeros)) + 0.5));
  }
  if (data_.empty()) {
    return 0;
  }
  int64_t zeros = 0;
  double sum = 0;
  for (const uint8_t rho_w : data_) {
    zeros += rho_w == 0 ? 1 : 0;
    sum += std::ldexp(1.0, -rho_w);
  }
  const auto m = static_cast<double>(data_.size());
  if (zeros > 0) {
    const double linear = m * std::log(m / static_cast<double>(zeros));
    if (linear <= static_cast<double>(LinearCountingThreshold(precision_))) {
      return static_cast<int64_t>(std::floor(linear + 0.5));
    }
  }
  const double estimate = Alpha(precision_) * m * m / sum;
  return static_cast<int64_t>(std::floor(estimate - EstimateBias(estimate, precision_) + 0.5));
}

std::size_t HyperLogLogPlusPlus::MaxSparseDataBytes() const {
  return static_cast<std::size_t>(std::ldexp(kMaximumSparseDataFraction, precision_));
}

std::size_t HyperLogLogPlusPlus::MaxBufferElements() const {
  return static_cast<std::size_t>(std::ldexp(kMaximumBufferElementsFraction, precision_));
}

std::vector<uint32_t> HyperLogLogPlusPlus::SortedSparseValues() const {
  std::vector<uint32_t> buffer(buffer_.begin(), buffer_.end());
  std::ranges::sort(buffer);
  return MergeSorted(sparse_data_, buffer);
}

HyperLogLogPlusPlus::MergeResult HyperLogLogPlusPlus::AddSparseValue(Sparse encoding,
                                                                     uint32_t value) {
  if (normal_) {
    MaybeDowngradeNormal(encoding);
    EnsureData();
    AddNormalSparseValue(encoding, value);
    return MergeResult::kMerged;
  }
  const Sparse own = Encoding();
  if (!own.IsCompatible(encoding)) {
    return MergeResult::kIncompatiblePrecisions;
  }
  if (encoding.IsLessThan(own)) {
    Downgrade(encoding);
    return AddSparseValue(encoding, value);
  }
  buffer_.insert(own.IsLessThan(encoding) ? encoding.Downgrade(value, own) : value);
  UpdateRepresentation();
  return MergeResult::kMerged;
}

HyperLogLogPlusPlus::MergeResult HyperLogLogPlusPlus::AddSparseValues(
    Sparse encoding, const std::vector<uint32_t>& sorted) {
  const Sparse own = Encoding();
  if (!own.IsCompatible(encoding)) {
    return MergeResult::kIncompatiblePrecisions;
  }
  if (encoding.IsLessThan(own)) {
    Downgrade(encoding);
    if (normal_) {
      MaybeDowngradeNormal(encoding);
      if (!sorted.empty()) {
        EnsureData();
      }
      for (const uint32_t value : sorted) {
        AddNormalSparseValue(encoding, value);
      }
      return MergeResult::kMerged;
    }
    return AddSparseValues(encoding, sorted);
  }
  if (sorted.empty()) {
    return MergeResult::kMerged;
  }
  if (own.IsLessThan(encoding)) {
    // Each value is added on its own, as the representation may turn normal along the way.
    for (const uint32_t value : sorted) {
      const MergeResult result = AddSparseValue(own, encoding.Downgrade(value, own));
      if (result != MergeResult::kMerged) {
        return result;
      }
    }
    return MergeResult::kMerged;
  }
  SetSparseData(own.Dedupe(MergeSorted(SortedSparseValues(), sorted)));
  UpdateRepresentation();
  return MergeResult::kMerged;
}

// Turns this sparse representation into one at the precisions of `encoding`, which are at most
// these, and adds its values again.
void HyperLogLogPlusPlus::Downgrade(Sparse encoding) {
  const Sparse old = Encoding();
  const std::vector<uint32_t> values = SortedSparseValues();
  precision_ = encoding.precision;
  sparse_precision_ = encoding.sparse_precision;
  has_sparse_data_ = false;
  sparse_data_.clear();
  sparse_data_bytes_ = 0;
  buffer_.clear();
  // zetasketch adds the buffer without downgrading its values; they are downgraded here too.
  for (const uint32_t value : values) {
    AddSparseValue(encoding, old.Downgrade(value, encoding));
  }
}

void HyperLogLogPlusPlus::FlushBuffer() {
  if (!buffer_.empty()) {
    SetSparseData(Encoding().Dedupe(SortedSparseValues()));
  }
}

void HyperLogLogPlusPlus::SetSparseData(std::vector<uint32_t> values) {
  sparse_data_bytes_ = DifferenceEncodedSize(values);
  sparse_size_ = static_cast<int>(values.size());
  sparse_data_ = std::move(values);
  has_sparse_data_ = true;
  buffer_.clear();
}

void HyperLogLogPlusPlus::UpdateRepresentation() {
  if (buffer_.size() > MaxBufferElements()) {
    FlushBuffer();
  }
  if (has_sparse_data_ && sparse_data_bytes_ >= MaxSparseDataBytes()) {
    Normalize();
  }
}

void HyperLogLogPlusPlus::Normalize() {
  const Sparse encoding = Encoding();
  normal_ = true;
  EnsureData();
  for (const uint32_t value : sparse_data_) {
    AddNormalSparseValue(encoding, value);
  }
  for (const uint32_t value : buffer_) {
    AddNormalSparseValue(encoding, value);
  }
  has_sparse_data_ = false;
  sparse_data_.clear();
  sparse_size_ = 0;
  sparse_data_bytes_ = 0;
  buffer_.clear();
}

void HyperLogLogPlusPlus::EnsureData() {
  if (data_.empty()) {
    data_.assign(std::size_t{1} << static_cast<unsigned>(precision_), 0);
  }
}

void HyperLogLogPlusPlus::MaybeDowngradeNormal(Sparse encoding) {
  if (precision_ > encoding.precision) {
    const std::vector<uint8_t> source = std::move(data_);
    const int source_precision = precision_;
    precision_ = encoding.precision;
    data_.assign(std::size_t{1} << static_cast<unsigned>(precision_), 0);
    MergeNormalData(source, source_precision);
  }
  sparse_precision_ = std::min(sparse_precision_, encoding.sparse_precision);
}

void HyperLogLogPlusPlus::AddNormalSparseValue(Sparse encoding, uint32_t value) {
  const uint32_t index = encoding.NormalIndex(value);
  const uint8_t rho_w =
      DowngradeRhoW(index, encoding.NormalRhoW(value), encoding.precision, precision_);
  uint8_t& register_value =
      data_.at(index >> static_cast<unsigned>(encoding.precision - precision_));
  register_value = std::max(register_value, rho_w);
}

void HyperLogLogPlusPlus::MergeNormalData(const std::vector<uint8_t>& source,
                                          int source_precision) {
  if (source.empty()) {
    return;
  }
  EnsureData();
  const auto shift = static_cast<unsigned>(source_precision - precision_);
  for (std::size_t index = 0; index < source.size(); ++index) {
    const uint8_t rho_w = source.at(index);
    // A register of 0 is unset, at any precision.
    if (rho_w == 0) {
      continue;
    }
    const uint8_t downgraded =
        DowngradeRhoW(static_cast<uint32_t>(index), rho_w, source_precision, precision_);
    uint8_t& register_value = data_.at(index >> shift);
    register_value = std::max(register_value, downgraded);
  }
}

}  // namespace zetasketch
