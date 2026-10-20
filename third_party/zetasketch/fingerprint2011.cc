// Copyright 2011 Google Inc. All Rights Reserved.
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
// Ported to C++ from Guava's com/google/common/hash/Fingerprint2011.java; see README.md.

#include "third_party/zetasketch/fingerprint2011.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace zetasketch {
namespace {

constexpr uint64_t kK0 = 0xa5b85c5e198ed849ULL;
constexpr uint64_t kK1 = 0x8d58ac26afe12e47ULL;
constexpr uint64_t kK2 = 0xc47b6e9e3a970ed3ULL;
constexpr uint64_t kK3 = 0xc6a4a7935bd1e995ULL;

// The little-endian value of the `length` bytes at `offset`, at most 8.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): Guava's order.
uint64_t Load(std::string_view bytes, std::size_t offset, std::size_t length = 8) {
  uint64_t value = 0;
  for (std::size_t i = 0; i < length; ++i) {
    value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes.at(offset + i))) << (8 * i);
  }
  return value;
}

uint64_t ShiftMix(uint64_t value) { return value ^ (value >> 47U); }

uint64_t Hash128To64(uint64_t high, uint64_t low) {
  uint64_t a = (low ^ high) * kK3;
  a ^= a >> 47U;
  uint64_t b = (high ^ a) * kK3;
  b ^= b >> 47U;
  return b * kK3;
}

// The two halves of a hash of the 32 bytes at `offset`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): Guava's order.
std::array<uint64_t, 2> WeakHashLength32WithSeeds(std::string_view bytes, std::size_t offset,
                                                  uint64_t seed_a, uint64_t seed_b) {
  const uint64_t part1 = Load(bytes, offset);
  const uint64_t part2 = Load(bytes, offset + 8);
  const uint64_t part3 = Load(bytes, offset + 16);
  const uint64_t part4 = Load(bytes, offset + 24);
  seed_a += part1;
  seed_b = std::rotr(seed_b + seed_a + part4, 51);
  const uint64_t c = seed_a;
  seed_a += part2;
  seed_a += part3;
  seed_b += std::rotr(seed_a, 23);
  return {seed_a + part4, seed_b + c};
}

uint64_t FullFingerprint(std::string_view bytes) {
  std::size_t offset = 0;
  std::size_t length = bytes.size();
  uint64_t x = Load(bytes, offset);
  uint64_t y = Load(bytes, offset + length - 16) ^ kK1;
  uint64_t z = Load(bytes, offset + length - 56) ^ kK0;
  auto v = WeakHashLength32WithSeeds(bytes, offset + length - 64, length, y);
  auto w = WeakHashLength32WithSeeds(bytes, offset + length - 32, length * kK1, kK0);
  z += ShiftMix(v.at(1)) * kK1;
  x = std::rotr(z + x, 39) * kK1;
  y = std::rotr(y, 33) * kK1;
  // Hashes 64 bytes at a time, leaving out the last ones, which the seeds above cover; at least
  // 64 bytes come first.
  length = (length - 1) & ~static_cast<std::size_t>(63);
  while (length != 0) {
    x = std::rotr(x + y + v.at(0) + Load(bytes, offset + 16), 37) * kK1;
    y = std::rotr(y + v.at(1) + Load(bytes, offset + 48), 42) * kK1;
    x ^= w.at(1);
    y ^= v.at(0);
    z = std::rotr(z ^ w.at(0), 33);
    v = WeakHashLength32WithSeeds(bytes, offset, v.at(1) * kK1, x + w.at(0));
    w = WeakHashLength32WithSeeds(bytes, offset + 32, z + w.at(1), y);
    std::swap(x, z);
    offset += 64;
    length -= 64;
  }
  return Hash128To64(Hash128To64(v.at(0), w.at(0)) + (ShiftMix(y) * kK1) + z,
                     Hash128To64(v.at(1), w.at(1)) + x);
}

uint64_t HashLength33To64(std::string_view bytes) {
  const std::size_t length = bytes.size();
  uint64_t z = Load(bytes, 24);
  uint64_t a = Load(bytes, 0) + ((length + Load(bytes, length - 16)) * kK0);
  uint64_t b = std::rotr(a + z, 52);
  uint64_t c = std::rotr(a, 37);
  a += Load(bytes, 8);
  c += std::rotr(a, 7);
  a += Load(bytes, 16);
  const uint64_t vf = a + z;
  const uint64_t vs = b + std::rotr(a, 31) + c;
  a = Load(bytes, 16) + Load(bytes, length - 32);
  z = Load(bytes, length - 8);
  b = std::rotr(a + z, 52);
  c = std::rotr(a, 37);
  a += Load(bytes, length - 24);
  c += std::rotr(a, 7);
  a += Load(bytes, length - 16);
  const uint64_t wf = a + z;
  const uint64_t ws = b + std::rotr(a, 31) + c;
  const uint64_t r = ShiftMix(((vf + ws) * kK2) + ((wf + vs) * kK0));
  return ShiftMix((r * kK0) + vs) * kK2;
}

uint64_t MurmurHash64WithSeed(std::string_view bytes, uint64_t seed) {
  const std::size_t length = bytes.size();
  const std::size_t aligned = length & ~static_cast<std::size_t>(7);
  uint64_t hash = seed ^ (length * kK3);
  for (std::size_t i = 0; i < aligned; i += 8) {
    hash ^= ShiftMix(Load(bytes, i) * kK3) * kK3;
    hash *= kK3;
  }
  if (aligned != length) {
    hash ^= Load(bytes, aligned, length - aligned);
    hash *= kK3;
  }
  return ShiftMix(ShiftMix(hash) * kK3);
}

}  // namespace

uint64_t Fingerprint2011(std::string_view bytes) {
  const std::size_t length = bytes.size();
  uint64_t result = length <= 32   ? MurmurHash64WithSeed(bytes, kK0 ^ kK1 ^ kK2)
                    : length <= 64 ? HashLength33To64(bytes)
                                   : FullFingerprint(bytes);
  const uint64_t u = length >= 8 ? Load(bytes, 0) : kK0;
  const uint64_t v = length >= 9 ? Load(bytes, length - 8) : kK0;
  result = Hash128To64(result + v, u);
  // Fingerprints 0 and 1 are reserved.
  return result == 0 || result == 1 ? result - 2 : result;
}

}  // namespace zetasketch
