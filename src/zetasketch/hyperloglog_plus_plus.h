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
// Ported to C++ from zetasketch's HyperLogLogPlusPlus.java and the classes it uses; see README.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace zetasketch {

// A HyperLogLog++ aggregator, serialized as an AggregatorStateProto of type
// HYPERLOGLOG_PLUS_UNIQUE in encoding version 2. It holds either the sparse representation, a
// sorted list of encoded hashes, or the normal one, a register per bucket.
class HyperLogLogPlusPlus {
 public:
  static constexpr int kMinimumPrecision = 4;
  static constexpr int kMaximumPrecision = 24;
  static constexpr int kMaximumSparsePrecision = 25;

  enum class MergeResult : uint8_t {
    kMerged,
    // The aggregators hold values of different types.
    kIncompatibleTypes,
    // Neither aggregator's precisions are both at most the other's.
    kIncompatiblePrecisions,
  };

  // An empty aggregator of values of `value_type`, a DefaultOpsType.Id or a custom type, with a
  // normal and a sparse precision; a sparse precision of 0 disables the sparse representation.
  HyperLogLogPlusPlus(int precision, int sparse_precision, int32_t value_type);

  // The aggregator that `sketch` serializes, or nullopt when it is not a valid one.
  static std::optional<HyperLogLogPlusPlus> Parse(std::string_view sketch);

  // Adds a value of the aggregator's type by its 64-bit hash.
  void AddHash(uint64_t hash);

  // Merges `other` into this aggregator, at the lower of the two precisions.
  MergeResult Merge(const HyperLogLogPlusPlus& other);

  // The serialized aggregator, which turns into the normal representation first where the sparse
  // one has outgrown it.
  std::string Serialize();

  // The estimated number of distinct values.
  int64_t Estimate();

 private:
  // How a sparse value encodes a hash's sparse index and rhoW for a pair of precisions.
  struct Sparse;

  HyperLogLogPlusPlus() = default;

  [[nodiscard]] Sparse Encoding() const;
  [[nodiscard]] std::size_t MaxSparseDataBytes() const;
  [[nodiscard]] std::size_t MaxBufferElements() const;
  // The values of the sparse data and the buffer, sorted but not deduplicated.
  [[nodiscard]] std::vector<uint32_t> SortedSparseValues() const;

  // The sparse representation.
  MergeResult AddSparseValue(Sparse encoding, uint32_t value);
  MergeResult AddSparseValues(Sparse encoding, const std::vector<uint32_t>& sorted);
  void Downgrade(Sparse encoding);
  void FlushBuffer();
  void SetSparseData(std::vector<uint32_t> values);
  void UpdateRepresentation();
  void Normalize();

  // The normal representation.
  void EnsureData();
  void MaybeDowngradeNormal(Sparse encoding);
  void AddNormalSparseValue(Sparse encoding, uint32_t value);
  void MergeNormalData(const std::vector<uint8_t>& source, int source_precision);

  int32_t value_type_ = 0;
  int64_t num_values_ = 0;
  int precision_ = 0;
  int sparse_precision_ = 0;
  bool normal_ = false;
  // The normal representation's registers, 2^precision_ of them, or empty for none.
  std::vector<uint8_t> data_;
  // Whether the sparse data is set, if empty, which serializes the field.
  bool has_sparse_data_ = false;
  // The sparse data, sorted and deduplicated, and its number of values and encoded bytes.
  std::vector<uint32_t> sparse_data_;
  int sparse_size_ = 0;
  std::size_t sparse_data_bytes_ = 0;
  // Sparse values not yet in the sparse data.
  std::unordered_set<uint32_t> buffer_;
};

}  // namespace zetasketch
