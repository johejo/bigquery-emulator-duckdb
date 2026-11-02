#include "src/unordered_map_order.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Follows libc++ 21's std::hash<std::string> (__murmur2_or_cityhash<size_t, 64>, without
// _LIBCPP_ABI_FIX_CITYHASH_IMPLEMENTATION) and its __hash_table: prime bucket counts, a load
// factor of 1, and a single list in which a key goes first in its bucket, and a key of an empty
// bucket goes first in the list.

namespace bigquery_emulator_duckdb {
namespace {

constexpr uint64_t kK0 = 0xc3a5c85c97cb3127ULL;
constexpr uint64_t kK1 = 0xb492b66fbe98f273ULL;
constexpr uint64_t kK2 = 0x9ae16a3b2f90404fULL;
constexpr uint64_t kK3 = 0xc949d7c7509e6557ULL;

template <typename T>
T Load(const char* s) {
  T value = 0;
  std::memcpy(&value, s, sizeof(value));
  return value;
}

uint64_t Rotate(uint64_t value, unsigned shift) {
  return shift == 0 ? value : ((value >> shift) | (value << (64U - shift)));
}

uint64_t ShiftMix(uint64_t value) { return value ^ (value >> 47U); }

uint64_t HashLen16(uint64_t u, uint64_t v) {
  constexpr uint64_t kMul = 0x9ddfea08eb382d69ULL;
  uint64_t a = (u ^ v) * kMul;
  a ^= (a >> 47U);
  uint64_t b = (v ^ a) * kMul;
  b ^= (b >> 47U);
  return b * kMul;
}

uint64_t HashLen0To16(const char* s, uint64_t len) {
  if (len > 8) {
    const auto a = Load<uint64_t>(s);
    const auto b = Load<uint64_t>(s + len - 8);
    const auto shift = static_cast<unsigned>(len);
    return HashLen16(a, ((b + len) >> shift) | ((b + len) << (64U - shift))) ^ b;
  }
  if (len >= 4) {
    const auto a = Load<uint32_t>(s);
    const auto b = Load<uint32_t>(s + len - 4);
    // libc++ shifts the 32-bit word before widening it.
    return HashLen16(len + static_cast<uint32_t>(a << 3U), b);
  }
  if (len > 0) {
    const auto a = static_cast<unsigned char>(s[0]);
    const auto b = static_cast<unsigned char>(s[len >> 1U]);
    const auto c = static_cast<unsigned char>(s[len - 1]);
    const uint32_t y = static_cast<uint32_t>(a) + (static_cast<uint32_t>(b) << 8U);
    const uint32_t z = static_cast<uint32_t>(len) + (static_cast<uint32_t>(c) << 2U);
    return ShiftMix((y * kK2) ^ (z * kK3)) * kK2;
  }
  return kK2;
}

uint64_t HashLen17To32(const char* s, uint64_t len) {
  const uint64_t a = Load<uint64_t>(s) * kK1;
  const auto b = Load<uint64_t>(s + 8);
  const uint64_t c = Load<uint64_t>(s + len - 8) * kK2;
  const uint64_t d = Load<uint64_t>(s + len - 16) * kK0;
  return HashLen16(Rotate(a - b, 43U) + Rotate(c, 30U) + d, a + Rotate(b ^ kK3, 20U) - c + len);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): CityHash's words and seeds, in order.
std::pair<uint64_t, uint64_t> WeakHashLen32WithSeeds(uint64_t w, uint64_t x, uint64_t y, uint64_t z,
                                                     uint64_t a, uint64_t b) {
  a += w;
  b = Rotate(b + a + z, 21U);
  const uint64_t c = a;
  a += x;
  a += y;
  b += Rotate(a, 44U);
  return {a + z, b + c};
}

std::pair<uint64_t, uint64_t> WeakHashLen32WithSeeds(const char* s, uint64_t a, uint64_t b) {
  return WeakHashLen32WithSeeds(Load<uint64_t>(s), Load<uint64_t>(s + 8), Load<uint64_t>(s + 16),
                                Load<uint64_t>(s + 24), a, b);
}

uint64_t HashLen33To64(const char* s, uint64_t len) {
  auto z = Load<uint64_t>(s + 24);
  uint64_t a = Load<uint64_t>(s) + ((len + Load<uint64_t>(s + len - 16)) * kK0);
  uint64_t b = Rotate(a + z, 52U);
  uint64_t c = Rotate(a, 37U);
  a += Load<uint64_t>(s + 8);
  c += Rotate(a, 7U);
  a += Load<uint64_t>(s + 16);
  const uint64_t vf = a + z;
  const uint64_t vs = b + Rotate(a, 31U) + c;
  a = Load<uint64_t>(s + 16) + Load<uint64_t>(s + len - 32);
  z += Load<uint64_t>(s + len - 8);
  b = Rotate(a + z, 52U);
  c = Rotate(a, 37U);
  a += Load<uint64_t>(s + len - 24);
  c += Rotate(a, 7U);
  a += Load<uint64_t>(s + len - 16);
  const uint64_t wf = a + z;
  const uint64_t ws = b + Rotate(a, 31U) + c;
  const uint64_t r = ShiftMix(((vf + ws) * kK2) + ((wf + vs) * kK0));
  return ShiftMix((r * kK0) + vs) * kK2;
}

uint64_t Hash(std::string_view key) {
  const char* s = key.data();
  uint64_t len = key.size();
  if (len <= 16) {
    return HashLen0To16(s, len);
  }
  if (len <= 32) {
    return HashLen17To32(s, len);
  }
  if (len <= 64) {
    return HashLen33To64(s, len);
  }
  auto x = Load<uint64_t>(s + len - 40);
  uint64_t y = Load<uint64_t>(s + len - 16) + Load<uint64_t>(s + len - 56);
  uint64_t z = HashLen16(Load<uint64_t>(s + len - 48) + len, Load<uint64_t>(s + len - 24));
  std::pair<uint64_t, uint64_t> v = WeakHashLen32WithSeeds(s + len - 64, len, z);
  std::pair<uint64_t, uint64_t> w = WeakHashLen32WithSeeds(s + len - 32, y + kK1, x);
  x = (x * kK1) + Load<uint64_t>(s);
  len = (len - 1) & ~uint64_t{63};
  while (true) {
    x = Rotate(x + y + v.first + Load<uint64_t>(s + 8), 37U) * kK1;
    y = Rotate(y + v.second + Load<uint64_t>(s + 48), 42U) * kK1;
    x ^= w.second;
    y += v.first + Load<uint64_t>(s + 40);
    z = Rotate(z + w.first, 33U) * kK1;
    v = WeakHashLen32WithSeeds(s, v.second * kK1, x + w.first);
    w = WeakHashLen32WithSeeds(s + 32, z + w.second, y + Load<uint64_t>(s + 16));
    std::swap(z, x);
    s += 64;
    len -= 64;
    if (len == 0) {
      break;
    }
  }
  return HashLen16(HashLen16(v.first, w.first) + (ShiftMix(y) * kK1) + z,
                   HashLen16(v.second, w.second) + x);
}

size_t NextPrime(size_t n) {
  for (;; ++n) {
    bool prime = n >= 2;
    for (size_t d = 2; prime && d * d <= n; ++d) {
      prime = n % d != 0;
    }
    if (prime) {
      return n;
    }
  }
}

size_t Constrain(uint64_t hash, size_t count) {
  return (count & (count - 1U)) == 0 ? hash & (count - 1U) : hash % count;
}

// The list starts at node 0, which holds no key; a bucket holds the node before its first.
class Table {
 public:
  bool Insert(const std::string& key) {
    const uint64_t hash = Hash(key);
    if (std::any_of(nodes_.begin() + 1, nodes_.end(),
                    [&](const Node& node) { return node.key == key; })) {
      return false;
    }
    const size_t size = nodes_.size() - 1;
    if (buckets_.empty() || size + 1 > buckets_.size()) {
      const size_t count = buckets_.size();
      const bool power2 = count > 2 && (count & (count - 1U)) == 0;
      Rehash(std::max<size_t>((2 * count) + (power2 ? 0 : 1), size + 1));
    }
    const size_t bucket = Constrain(hash, buckets_.size());
    const size_t node = nodes_.size();
    nodes_.push_back({.key = key, .hash = hash});
    if (buckets_.at(bucket) == kNone) {
      nodes_.at(node).next = nodes_.front().next;
      nodes_.front().next = node;
      buckets_.at(bucket) = 0;
      if (nodes_.at(node).next != kNone) {
        buckets_.at(Constrain(nodes_.at(nodes_.at(node).next).hash, buckets_.size())) = node;
      }
    } else {
      const size_t before = buckets_.at(bucket);
      nodes_.at(node).next = nodes_.at(before).next;
      nodes_.at(before).next = node;
    }
    return true;
  }

  [[nodiscard]] std::vector<std::string> Keys() const {
    std::vector<std::string> keys;
    for (size_t node = nodes_.front().next; node != kNone; node = nodes_.at(node).next) {
      keys.push_back(nodes_.at(node).key);
    }
    return keys;
  }

 private:
  static constexpr size_t kNone = std::numeric_limits<size_t>::max();

  struct Node {
    std::string key;
    uint64_t hash = 0;
    size_t next = kNone;
  };

  // __hash_table::__rehash for unique keys, which only grows the table here.
  void Rehash(size_t count) {
    if (count == 1) {
      count = 2;
    } else if ((count & (count - 1U)) != 0) {
      count = NextPrime(count);
    }
    if (count <= buckets_.size()) {
      return;
    }
    buckets_.assign(count, kNone);
    size_t before = 0;
    size_t current = nodes_.front().next;
    if (current == kNone) {
      return;
    }
    size_t previous_bucket = Constrain(nodes_.at(current).hash, count);
    buckets_.at(previous_bucket) = before;
    for (before = current, current = nodes_.at(current).next; current != kNone;
         current = nodes_.at(before).next) {
      const size_t bucket = Constrain(nodes_.at(current).hash, count);
      if (bucket == previous_bucket) {
        before = current;
      } else if (buckets_.at(bucket) == kNone) {
        buckets_.at(bucket) = before;
        before = current;
        previous_bucket = bucket;
      } else {
        nodes_.at(before).next = nodes_.at(current).next;
        nodes_.at(current).next = nodes_.at(buckets_.at(bucket)).next;
        nodes_.at(buckets_.at(bucket)).next = current;
      }
    }
  }

  std::vector<Node> nodes_ = {Node{}};
  std::vector<size_t> buckets_;
};

}  // namespace

std::vector<std::string> UnorderedMapOrder(const std::vector<std::string>& keys) {
  Table table;
  for (const std::string& key : keys) {
    table.Insert(key);
  }
  return table.Keys();
}

}  // namespace bigquery_emulator_duckdb
