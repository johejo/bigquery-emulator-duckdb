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
// Ported to C++ from Guava's com/google/common/hash/Fingerprint2011Test.java, whose fingerprints
// cover every branch of the hash; see README.md.

#include "src/zetasketch/fingerprint2011.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace zetasketch {
namespace {

TEST(Fingerprint2011Test, ReallySimpleFingerprints) {
  std::string test;
  for (int i = 0; i < 64; ++i) {
    test += "test";
  }
  EXPECT_EQ(Fingerprint2011("test"), 8473225671271759044ULL);
  EXPECT_EQ(Fingerprint2011(std::string_view(test).substr(0, 32)), 7345148637025587076ULL);
  EXPECT_EQ(Fingerprint2011(test), 4904844928629814570ULL);
}

// Fingerprints of prefixes of a buffer that the fingerprints themselves fill, up to 3200 bytes.
TEST(Fingerprint2011Test, MultipleLengths) {
  constexpr std::size_t kIterations = 800;
  std::string buffer(kIterations * 4, '\0');
  std::size_t length = 0;
  uint64_t h = 0;
  const auto mix = [&](std::size_t prefix) {
    h ^= Fingerprint2011(std::string_view(buffer).substr(0, prefix));
    h ^= h >> 41U;
    h *= 949921979;
    buffer.at(length++) = static_cast<char>('a' + ((h & 0xFFFFFU) % 26));
  };
  for (std::size_t i = 0; i < kIterations; ++i) {
    mix(i);
    mix(i * i % length);
    mix(i * i * i % length);
    mix(length);
    const auto byte = [&](std::size_t index) {
      return static_cast<std::size_t>(static_cast<uint8_t>(buffer.at(index)));
    };
    const std::size_t x0 = byte(length - 1);
    const std::size_t x1 = byte(length - 2);
    const std::size_t x2 = byte(length - 3);
    const std::size_t x3 = byte(length / 2);
    const auto flip = [&](std::size_t index, std::size_t bits) {
      buffer.at(index % length) = static_cast<char>(byte(index % length) ^ bits);
    };
    flip((x0 << 16U) + (x1 << 8U) + x2, x3);
    flip((x1 << 16U) + (x2 << 8U) + x3, i % 256);
  }
  EXPECT_EQ(h, 0xeaa3b1c985261632ULL);
}

}  // namespace
}  // namespace zetasketch
