#include <openssl/aead.h>
#include <openssl/aes.h>
#include <openssl/base64.h>
#include <openssl/cmac.h>
#include <openssl/mem.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "nlohmann/json.hpp"
#include "utf8_validity.h"

// The AEAD encryption functions, which take serialized Tink keysets and produce and read Tink's
// ciphertexts: the output prefix of the key, then what the key's primitive makes. AEAD.ENCRYPT
// uses AES-GCM keys, which make a 12-byte IV, and the encrypted data followed by a 16-byte tag;
// DETERMINISTIC_ENCRYPT uses AES-SIV keys (RFC 5297), which make a 16-byte synthetic IV followed
// by the encrypted data. The KEYS functions make and change keysets of these keys.

#include "src/backend_functions/aead.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

constexpr std::string_view kAesGcmKey = "type.googleapis.com/google.crypto.tink.AesGcmKey";
constexpr std::string_view kAesSivKey = "type.googleapis.com/google.crypto.tink.AesSivKey";
constexpr std::size_t kIvSize = 12;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kBlockSize = 16;

// Tink's KeyMaterialType, KeyStatusType and OutputPrefixType.
constexpr uint64_t kSymmetric = 1;
constexpr uint64_t kEnabled = 1;
constexpr uint64_t kTink = 1;
constexpr uint64_t kLegacy = 2;
constexpr uint64_t kRaw = 3;
constexpr uint64_t kCrunchy = 4;

// The names of Tink's enums in JSON keysets, by value.
constexpr std::array<std::string_view, 5> kKeyMaterialTypes = {
    "UNKNOWN_KEYMATERIAL", "SYMMETRIC", "ASYMMETRIC_PRIVATE", "ASYMMETRIC_PUBLIC", "REMOTE",
};
constexpr std::array<std::string_view, 4> kStatuses = {
    "UNKNOWN_STATUS",
    "ENABLED",
    "DISABLED",
    "DESTROYED",
};
constexpr std::array<std::string_view, 5> kOutputPrefixTypes = {
    "UNKNOWN_PREFIX", "TINK", "LEGACY", "RAW", "CRUNCHY",
};

// A field of a serialized protocol buffer message, with either `varint` or `bytes`.
struct Field {
  uint64_t number = 0;
  uint64_t varint = 0;
  std::string_view bytes = {};
};

bool ReadVarint(std::string_view& data, uint64_t& value) {
  value = 0;
  for (unsigned shift = 0; shift < 64 && !data.empty(); shift += 7) {
    const auto byte = static_cast<uint8_t>(data.front());
    data.remove_prefix(1);
    value |= static_cast<uint64_t>(byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0) {
      return true;
    }
  }
  return false;
}

absl::Status Malformed() {
  return absl::OutOfRangeError("Could not parse the input stream as a Keyset-proto.");
}

// The fields of a serialized message, skipping fixed-size ones.
absl::StatusOr<std::vector<Field>> ParseMessage(std::string_view data) {
  std::vector<Field> fields;
  while (!data.empty()) {
    uint64_t tag = 0;
    if (!ReadVarint(data, tag)) {
      return Malformed();
    }
    Field field{.number = tag >> 3U};
    uint64_t size = 0;
    switch (tag & 7U) {
      case 0:
        if (!ReadVarint(data, field.varint)) {
          return Malformed();
        }
        fields.push_back(field);
        continue;
      case 1:
        size = 8;
        break;
      case 2:
        if (!ReadVarint(data, size) || size > data.size()) {
          return Malformed();
        }
        field.bytes = data.substr(0, size);
        fields.push_back(field);
        break;
      case 5:
        size = 4;
        break;
      default:
        return Malformed();
    }
    if (size > data.size()) {
      return Malformed();
    }
    data.remove_prefix(size);
  }
  return fields;
}

void WriteVarint(std::string& out, uint64_t value) {
  while (value >= 0x80U) {
    out.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
    value >>= 7U;
  }
  out.push_back(static_cast<char>(value));
}

// A varint field, which protocol buffers leave out when it is 0.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a field's number, then its value.
void WriteVarintField(std::string& out, uint64_t number, uint64_t value) {
  if (value != 0) {
    WriteVarint(out, number << 3U);
    WriteVarint(out, value);
  }
}

// A bytes field, which protocol buffers leave out when it is empty.
void WriteBytesField(std::string& out, uint64_t number, std::string_view value) {
  if (!value.empty()) {
    WriteVarint(out, (number << 3U) | 2U);
    WriteVarint(out, value.size());
    out += value;
  }
}

// A Keyset.Key, with its KeyData.
struct KeysetKey {
  std::string type_url;
  // The serialized key of the type.
  std::string value;
  uint64_t key_material_type = 0;
  uint64_t status = 0;
  uint32_t id = 0;
  uint64_t output_prefix_type = 0;
};

// A Tink Keyset: the ID of its primary key, then its keys.
struct Keyset {
  uint32_t primary_id = 0;
  std::vector<KeysetKey> keys;
};

absl::StatusOr<KeysetKey> ParseKeysetKey(std::string_view data) {
  const auto fields = ParseMessage(data);
  if (!fields.ok()) {
    return fields.status();
  }
  KeysetKey key;
  std::string_view key_data;
  for (const Field& field : *fields) {
    if (field.number == 1) {
      key_data = field.bytes;
    } else if (field.number == 2) {
      key.status = field.varint;
    } else if (field.number == 3) {
      key.id = static_cast<uint32_t>(field.varint);
    } else if (field.number == 4) {
      key.output_prefix_type = field.varint;
    }
  }
  const auto key_fields = ParseMessage(key_data);
  if (!key_fields.ok()) {
    return key_fields.status();
  }
  for (const Field& field : *key_fields) {
    if (field.number == 1) {
      key.type_url = field.bytes;
    } else if (field.number == 2) {
      key.value = field.bytes;
    } else if (field.number == 3) {
      key.key_material_type = field.varint;
    }
  }
  return key;
}

absl::StatusOr<Keyset> ParseKeyset(std::string_view data) {
  const auto fields = ParseMessage(data);
  if (!fields.ok()) {
    return fields.status();
  }
  Keyset keyset;
  for (const Field& field : *fields) {
    if (field.number == 1) {
      keyset.primary_id = static_cast<uint32_t>(field.varint);
    } else if (field.number == 2) {
      auto key = ParseKeysetKey(field.bytes);
      if (!key.ok()) {
        return key.status();
      }
      keyset.keys.push_back(*std::move(key));
    }
  }
  return keyset;
}

std::string Serialize(const Keyset& keyset) {
  std::string out;
  WriteVarintField(out, 1, keyset.primary_id);
  for (const KeysetKey& key : keyset.keys) {
    std::string key_data;
    WriteBytesField(key_data, 1, key.type_url);
    WriteBytesField(key_data, 2, key.value);
    WriteVarintField(key_data, 3, key.key_material_type);
    std::string serialized;
    WriteBytesField(serialized, 1, key_data);
    WriteVarintField(serialized, 2, key.status);
    WriteVarintField(serialized, 3, key.id);
    WriteVarintField(serialized, 4, key.output_prefix_type);
    WriteBytesField(out, 2, serialized);
  }
  return out;
}

// The kind of keys a function encrypts or decrypts with, and its names in Tink's errors: its own,
// its mangled C++ type, and the mangled C++ types its keys support.
struct Primitive {
  std::string_view type_url;
  std::string_view name;
  std::string_view tink_type;
  std::string_view tink_types;
};

constexpr Primitive kAead = {
    .type_url = kAesGcmKey,
    .name = "AEAD",
    .tink_type = "N6crypto4tink4AeadE",
    .tink_types = "N6crypto4tink8CordAeadE, N6crypto4tink4AeadE",
};
constexpr Primitive kDeterministicAead = {
    .type_url = kAesSivKey,
    .name = "Deterministic AEAD",
    .tink_type = "N6crypto4tink17DeterministicAeadE",
    .tink_types = "N6crypto4tink17DeterministicAeadE",
};

// An enabled key of a keyset, ready to encrypt or decrypt.
struct Key {
  uint32_t id = 0;
  // The ciphertexts of the key start with this; empty for RAW keys.
  std::string prefix;
  // The AES-GCM key, or the 64-byte AES-SIV key.
  std::string value;
};

struct Keys {
  std::vector<Key> keys;
  // The key that encrypts.
  Key primary;
};

// The key value of a serialized AesGcmKey or AesSivKey: its version, then the key.
absl::StatusOr<std::string> KeyValue(const Primitive& primitive, std::string_view data) {
  const auto fields = ParseMessage(data);
  if (!fields.ok()) {
    return fields.status();
  }
  const bool gcm = primitive.type_url == kAesGcmKey;
  uint64_t version = 0;
  std::string value;
  for (const Field& field : *fields) {
    if (field.number == 1) {
      version = field.varint;
    } else if (field.number == (gcm ? 3U : 2U)) {
      value = field.bytes;
    }
  }
  const std::string_view type = gcm ? "AesGcmKey" : "AesSivKey";
  if (version != 0) {
    return absl::OutOfRangeError(std::string(type) + " has an unsupported version");
  }
  if (gcm ? value.size() != 16 && value.size() != 32 : value.size() != 64) {
    return absl::OutOfRangeError(std::string(type) + " has an invalid key size");
  }
  return value;
}

// The enabled keys of a serialized keyset, which have to be of the primitive's type.
absl::StatusOr<Keys> ParseKeys(const Primitive& primitive, std::string_view data) {
  const auto keyset = ParseKeyset(data);
  if (!keyset.ok()) {
    return keyset.status();
  }
  if (keyset->keys.empty()) {
    return absl::OutOfRangeError("A valid keyset must contain at least one key.");
  }
  Keys keys;
  for (const KeysetKey& keyset_key : keyset->keys) {
    if (keyset_key.status != kEnabled) {
      continue;
    }
    Key key{.id = keyset_key.id, .prefix = {}, .value = {}};
    switch (keyset_key.output_prefix_type) {
      case kTink:
      case kLegacy:
      case kCrunchy:
        key.prefix.push_back(keyset_key.output_prefix_type == kTink ? '\x01' : '\x00');
        for (int shift = 24; shift >= 0; shift -= 8) {
          key.prefix.push_back(static_cast<char>((key.id >> static_cast<unsigned>(shift)) & 0xffU));
        }
        break;
      case kRaw:
        break;
      default:
        return absl::OutOfRangeError("Key has an unknown output prefix type");
    }
    if (keyset_key.type_url != primitive.type_url) {
      const Primitive& other = primitive.type_url == kAesGcmKey ? kDeterministicAead : kAead;
      if (keyset_key.type_url == other.type_url) {
        return absl::OutOfRangeError("Primitive type " + std::string(primitive.tink_type) +
                                     " not among supported primitives " +
                                     std::string(other.tink_types) + " for type URL " +
                                     keyset_key.type_url);
      }
      return absl::OutOfRangeError("Key type is unsupported: " + keyset_key.type_url);
    }
    auto value = KeyValue(primitive, keyset_key.value);
    if (!value.ok()) {
      return value.status();
    }
    key.value = *std::move(value);
    keys.keys.push_back(std::move(key));
  }
  const auto primary =
      std::ranges::find_if(keys.keys, [&](const Key& key) { return key.id == keyset->primary_id; });
  if (primary == keys.keys.end()) {
    return absl::OutOfRangeError("Keyset has no enabled primary key");
  }
  keys.primary = *primary;
  return keys;
}

// Seals or opens `in` with the AES-GCM `key`, or returns nullopt when opening fails.
std::optional<std::string> Seal(const Key& key, std::string_view iv, std::string_view in,
                                std::string_view aad, bool open) {
  const EVP_AEAD* aead = key.value.size() == 16 ? EVP_aead_aes_128_gcm() : EVP_aead_aes_256_gcm();
  bssl::ScopedEVP_AEAD_CTX context;
  if (EVP_AEAD_CTX_init(context.get(), aead, reinterpret_cast<const uint8_t*>(key.value.data()),
                        key.value.size(), kTagSize, nullptr) != 1) {
    return std::nullopt;
  }
  std::string out(in.size() + (open ? 0 : kTagSize), '\0');
  std::size_t size = 0;
  auto* out_data = reinterpret_cast<uint8_t*>(out.data());
  const auto* iv_data = reinterpret_cast<const uint8_t*>(iv.data());
  const auto* in_data = reinterpret_cast<const uint8_t*>(in.data());
  const auto* aad_data = reinterpret_cast<const uint8_t*>(aad.data());
  const int ok = open ? EVP_AEAD_CTX_open(context.get(), out_data, &size, out.size(), iv_data,
                                          iv.size(), in_data, in.size(), aad_data, aad.size())
                      : EVP_AEAD_CTX_seal(context.get(), out_data, &size, out.size(), iv_data,
                                          iv.size(), in_data, in.size(), aad_data, aad.size());
  if (ok != 1) {
    return std::nullopt;
  }
  out.resize(size);
  return out;
}

using Block = std::array<uint8_t, kBlockSize>;

Block Cmac(std::string_view key, std::string_view in) {
  Block out{};
  AES_CMAC(out.data(), reinterpret_cast<const uint8_t*>(key.data()), key.size(),
           reinterpret_cast<const uint8_t*>(in.data()), in.size());
  return out;
}

// Multiplication by x in GF(2^128), RFC 5297's dbl.
Block Double(const Block& block) {
  Block out{};
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    const unsigned next = i + 1 < kBlockSize ? block.at(i + 1) >> 7U : 0;
    out.at(i) = static_cast<uint8_t>((static_cast<unsigned>(block.at(i)) << 1U) | next);
  }
  if ((block.at(0) & 0x80U) != 0) {
    out.at(kBlockSize - 1) ^= 0x87U;
  }
  return out;
}

void Xor(char& byte, uint8_t mask) { byte = static_cast<char>(static_cast<uint8_t>(byte) ^ mask); }

// RFC 5297's S2V of the additional data and the plaintext, as Tink computes it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the order of S2V's strings.
Block S2v(std::string_view key, std::string_view aad, std::string_view plaintext) {
  Block d = Cmac(key, std::string(kBlockSize, '\0'));
  const Block aad_mac = Cmac(key, aad);
  d = Double(d);
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    d.at(i) ^= aad_mac.at(i);
  }
  std::string t(plaintext);
  if (t.size() >= kBlockSize) {
    for (std::size_t i = 0; i < kBlockSize; ++i) {
      Xor(t.at(t.size() - kBlockSize + i), d.at(i));
    }
  } else {
    d = Double(d);
    t.push_back('\x80');
    t.resize(kBlockSize, '\0');
    for (std::size_t i = 0; i < kBlockSize; ++i) {
      Xor(t.at(i), d.at(i));
    }
  }
  return Cmac(key, t);
}

// AES-CTR with the second half of the AES-SIV `key`, from the synthetic IV `siv` with the bits
// RFC 5297 clears.
std::string Ctr(std::string_view key, const Block& siv, std::string_view in) {
  AES_KEY aes;
  AES_set_encrypt_key(reinterpret_cast<const uint8_t*>(key.data()) + (key.size() / 2),
                      (key.size() / 2) * 8, &aes);
  Block counter = siv;
  counter.at(8) &= 0x7fU;
  counter.at(12) &= 0x7fU;
  Block stream{};
  unsigned num = 0;
  std::string out(in.size(), '\0');
  AES_ctr128_encrypt(reinterpret_cast<const uint8_t*>(in.data()),
                     reinterpret_cast<uint8_t*>(out.data()), in.size(), &aes, counter.data(),
                     stream.data(), &num);
  return out;
}

std::string SivEncrypt(const Key& key, std::string_view plaintext, std::string_view aad) {
  const std::string_view mac_key = std::string_view(key.value).substr(0, key.value.size() / 2);
  const Block siv = S2v(mac_key, aad, plaintext);
  return std::string(siv.begin(), siv.end()) + Ctr(key.value, siv, plaintext);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the order of Seal's arguments.
std::optional<std::string> SivDecrypt(const Key& key, std::string_view ciphertext,
                                      std::string_view aad) {
  if (ciphertext.size() < kBlockSize) {
    return std::nullopt;
  }
  Block siv{};
  std::ranges::copy(ciphertext.substr(0, kBlockSize), siv.begin());
  std::string plaintext = Ctr(key.value, siv, ciphertext.substr(kBlockSize));
  const std::string_view mac_key = std::string_view(key.value).substr(0, key.value.size() / 2);
  const Block expected = S2v(mac_key, aad, plaintext);
  if (CRYPTO_memcmp(expected.data(), siv.data(), kBlockSize) != 0) {
    return std::nullopt;
  }
  return plaintext;
}

// An error of `function`, with the suffix BigQuery adds.
absl::Status Error(std::string_view function, std::string_view message,
                   std::string_view separator = "; error in ") {
  return absl::OutOfRangeError(std::string(message) + std::string(separator) +
                               std::string(function) + " expression");
}

absl::Status Failed(std::string_view function, std::string_view message) {
  return Error(function, std::string(function) + " failed: " + std::string(message));
}

// The keys of the first argument.
absl::StatusOr<Keys> KeysOf(std::string_view function, const Primitive& primitive,
                            const Arguments& arguments) {
  auto keys = ParseKeys(primitive, arguments.String(0));
  if (!keys.ok()) {
    return Failed(function, "Creation of " + std::string(primitive.name) +
                                " primitive failed: " + std::string(keys.status().message()));
  }
  return keys;
}

// AEAD.ENCRYPT(keyset, plaintext, additional_data).
absl::StatusOr<std::string> Encrypt(const Arguments& arguments) {
  constexpr std::string_view kFunction = "AEAD.ENCRYPT";
  const auto keys = KeysOf(kFunction, kAead, arguments);
  if (!keys.ok()) {
    return keys.status();
  }
  std::string iv(kIvSize, '\0');
  RAND_bytes(reinterpret_cast<uint8_t*>(iv.data()), iv.size());
  const auto sealed = Seal(keys->primary, iv, arguments.String(1), arguments.String(2), false);
  if (!sealed) {
    return Failed(kFunction, "encryption failed");
  }
  return keys->primary.prefix + iv + *sealed;
}

// DETERMINISTIC_ENCRYPT(keyset, plaintext, additional_data).
absl::StatusOr<std::string> DeterministicEncrypt(const Arguments& arguments) {
  const auto keys = KeysOf("DETERMINISTIC_ENCRYPT", kDeterministicAead, arguments);
  if (!keys.ok()) {
    return keys.status();
  }
  return keys->primary.prefix + SivEncrypt(keys->primary, arguments.String(1), arguments.String(2));
}

// AEAD.DECRYPT_* and DETERMINISTIC_DECRYPT_*(keyset, ciphertext, additional_data). Tries the
// keys whose prefix the ciphertext starts with, then the RAW keys, as Tink does.
absl::StatusOr<std::string> Decrypt(std::string_view function, const Primitive& primitive,
                                    const Arguments& arguments) {
  const auto keys = KeysOf(function, primitive, arguments);
  if (!keys.ok()) {
    return keys.status();
  }
  const std::string ciphertext = arguments.String(1);
  const std::string aad = arguments.String(2);
  for (const bool raw : {false, true}) {
    for (const Key& key : keys->keys) {
      if (key.prefix.empty() != raw || !ciphertext.starts_with(key.prefix)) {
        continue;
      }
      const std::string_view rest = std::string_view(ciphertext).substr(key.prefix.size());
      std::optional<std::string> plaintext;
      if (primitive.type_url == kAesSivKey) {
        plaintext = SivDecrypt(key, rest, aad);
      } else if (rest.size() >= kIvSize + kTagSize) {
        plaintext = Seal(key, rest.substr(0, kIvSize), rest.substr(kIvSize), aad, true);
      }
      if (plaintext) {
        return *std::move(plaintext);
      }
    }
  }
  return Failed(function, "decryption failed");
}

// A decryption to STRING, which fails for plaintext that is not UTF-8.
absl::StatusOr<std::string> DecryptString(std::string_view function, std::string_view bytes,
                                          const Primitive& primitive, const Arguments& arguments) {
  auto plaintext = Decrypt(function, primitive, arguments);
  if (plaintext.ok() && !utf8_range::IsStructurallyValid(*plaintext)) {
    return Failed(function,
                  "Decrypted plaintext is not a valid UTF-8 string. To decrypt to "
                  "BYTES, use " +
                      std::string(bytes));
  }
  return plaintext;
}

// A new random key ID that no key of `keyset` has.
uint32_t NewKeyId(const Keyset& keyset) {
  while (true) {
    uint32_t id = 0;
    RAND_bytes(reinterpret_cast<uint8_t*>(&id), sizeof(id));
    if (std::ranges::none_of(keyset.keys, [id](const KeysetKey& key) { return key.id == id; })) {
      return id;
    }
  }
}

// A new key of `key_type` for `keyset`, with a TINK prefix.
absl::StatusOr<KeysetKey> NewKey(std::string_view function, std::string_view key_type,
                                 const Keyset& keyset) {
  const bool gcm = key_type == "AEAD_AES_GCM_256";
  if (!gcm && key_type != "DETERMINISTIC_AEAD_AES_SIV_CMAC_256") {
    return absl::OutOfRangeError(std::string(function == "KEYS.NEW_KEYSET" ? "First" : "Second") +
                                 " argument (key type) in function " + std::string(function) +
                                 " must be one of 'AEAD_AES_GCM_256', "
                                 "'DETERMINISTIC_AEAD_AES_SIV_CMAC_256', but is '" +
                                 std::string(key_type) + "'");
  }
  std::string value(gcm ? 32 : 64, '\0');
  RAND_bytes(reinterpret_cast<uint8_t*>(value.data()), value.size());
  std::string key;
  WriteBytesField(key, gcm ? 3 : 2, value);
  return KeysetKey{
      .type_url = std::string(gcm ? kAesGcmKey : kAesSivKey),
      .value = std::move(key),
      .key_material_type = kSymmetric,
      .status = kEnabled,
      .id = NewKeyId(keyset),
      .output_prefix_type = kTink,
  };
}

// The keyset of the first argument of `function`, which may have no keys.
absl::StatusOr<Keyset> KeysetOf(std::string_view function, const Arguments& arguments) {
  auto keyset = ParseKeyset(arguments.String(0));
  if (!keyset.ok()) {
    return Error(function,
                 "Keyset deserialization failed: Error reading keyset data: " +
                     std::string(keyset.status().message()),
                 " Error in ");
  }
  return keyset;
}

// The key type of KEYS.NEW_KEYSET that makes keys of `type_url`.
std::string KeyType(std::string_view type_url) {
  if (type_url == kAesGcmKey) {
    return "AEAD_AES_GCM_256";
  }
  if (type_url == kAesSivKey) {
    return "DETERMINISTIC_AEAD_AES_SIV_CMAC_256";
  }
  return std::string(type_url);
}

// Adds `key` to `keyset`, as its primary key when `primary` is or the keyset has no keys.
std::string AddKey(Keyset keyset, KeysetKey key, bool primary) {
  if (primary || keyset.keys.empty()) {
    keyset.primary_id = key.id;
  }
  keyset.keys.push_back(std::move(key));
  return Serialize(keyset);
}

// KEYS.NEW_KEYSET(key_type), where key_type can't be NULL.
absl::StatusOr<std::string> NewKeyset(const Arguments& arguments) {
  constexpr std::string_view kFunction = "KEYS.NEW_KEYSET";
  Keyset keyset;
  auto key = NewKey(kFunction, arguments.IsNull(0) ? "" : arguments.String(0), keyset);
  if (!key.ok()) {
    return key.status();
  }
  return AddKey(std::move(keyset), *std::move(key), true);
}

// KEYS.ROTATE_KEYSET(keyset, key_type): a new primary key of the type of the keyset's keys.
absl::StatusOr<std::optional<std::string>> RotateKeyset(const Arguments& arguments) {
  constexpr std::string_view kFunction = "KEYS.ROTATE_KEYSET";
  if (arguments.IsNull(0)) {
    return std::nullopt;
  }
  auto keyset = KeysetOf(kFunction, arguments);
  if (!keyset.ok()) {
    return keyset.status();
  }
  auto key = NewKey(kFunction, arguments.IsNull(1) ? "" : arguments.String(1), *keyset);
  if (!key.ok()) {
    return key.status();
  }
  if (const auto other = std::ranges::find_if(
          keyset->keys,
          [&](const KeysetKey& existing) { return existing.type_url != key->type_url; });
      other != keyset->keys.end()) {
    return Error(kFunction, "The key type " + KeyType(other->type_url) +
                                " in original keyset doesn't match the incoming key type " +
                                KeyType(key->type_url));
  }
  return AddKey(*std::move(keyset), *std::move(key), true);
}

// KEYS.ADD_KEY_FROM_RAW_BYTES(keyset, 'AES_GCM', raw_key_bytes): a RAW AES-GCM key, which is
// primary only in a keyset that had no keys.
absl::StatusOr<std::string> AddAesGcmKey(const Arguments& arguments) {
  constexpr std::string_view kFunction = "KEYS.ADD_KEY_FROM_RAW_BYTES";
  auto keyset = KeysetOf(kFunction, arguments);
  if (!keyset.ok()) {
    return keyset.status();
  }
  const std::string raw = arguments.String(1);
  if (raw.size() != 16 && raw.size() != 32) {
    return Error(kFunction,
                 "Failed to add a key from raw bytes: Unsupported key size: " +
                     std::to_string(raw.size()) + " bytes; expected 16 or 32 bytes",
                 ". Error in ");
  }
  std::string value;
  WriteBytesField(value, 3, raw);
  KeysetKey key{
      .type_url = std::string(kAesGcmKey),
      .value = std::move(value),
      .key_material_type = kSymmetric,
      .status = kEnabled,
      .id = NewKeyId(*keyset),
      .output_prefix_type = kRaw,
  };
  return AddKey(*std::move(keyset), std::move(key), false);
}

// KEYS.KEYSET_LENGTH(keyset): the number of keys, whatever their status.
absl::StatusOr<int64_t> KeysetLength(const Arguments& arguments) {
  const auto keyset = KeysetOf("KEYS.KEYSET_LENGTH", arguments);
  if (!keyset.ok()) {
    return keyset.status();
  }
  return static_cast<int64_t>(keyset->keys.size());
}

template <std::size_t N>
nlohmann::json EnumName(const std::array<std::string_view, N>& names, uint64_t value) {
  if (value < N) {
    return std::string(names.at(value));
  }
  return value;
}

template <std::size_t N>
std::optional<uint64_t> EnumValue(const std::array<std::string_view, N>& names,
                                  const nlohmann::json& value) {
  if (value.is_number_unsigned()) {
    return value.get<uint64_t>();
  }
  if (!value.is_string()) {
    return std::nullopt;
  }
  const auto name = std::ranges::find(names, value.get<std::string>());
  if (name == names.end()) {
    return std::nullopt;
  }
  return name - names.begin();
}

std::string Base64(std::string_view data) {
  std::size_t size = 0;
  EVP_EncodedLength(&size, data.size());
  std::string out(size, '\0');
  out.resize(EVP_EncodeBlock(reinterpret_cast<uint8_t*>(out.data()),
                             reinterpret_cast<const uint8_t*>(data.data()), data.size()));
  return out;
}

// The decoded `data`, which is empty when `data` is not base64, as BigQuery takes it.
std::string FromBase64(std::string_view data) {
  std::size_t size = 0;
  if (EVP_DecodedLength(&size, data.size()) != 1) {
    return "";
  }
  std::string out(size, '\0');
  if (EVP_DecodeBase64(reinterpret_cast<uint8_t*>(out.data()), &size, out.size(),
                       reinterpret_cast<const uint8_t*>(data.data()), data.size()) != 1) {
    return "";
  }
  out.resize(size);
  return out;
}

// KEYS.KEYSET_TO_JSON(keyset): the keyset in Tink's JSON format, without spaces and with the
// fields of each object in alphabetical order, as nlohmann::json orders them.
absl::StatusOr<std::string> KeysetToJson(const Arguments& arguments) {
  constexpr std::string_view kFunction = "KEYS.KEYSET_TO_JSON";
  const auto keyset = ParseKeyset(arguments.String(0));
  if (!keyset.ok()) {
    return Error(kFunction, keyset.status().message(), " Error in ");
  }
  nlohmann::json keys = nlohmann::json::array();
  for (const KeysetKey& key : keyset->keys) {
    keys.push_back({
        {
            "keyData",
            {
                {"typeUrl", key.type_url},
                {"value", Base64(key.value)},
                {"keyMaterialType", EnumName(kKeyMaterialTypes, key.key_material_type)},
            },
        },
        {"status", EnumName(kStatuses, key.status)},
        {"keyId", key.id},
        {"outputPrefixType", EnumName(kOutputPrefixTypes, key.output_prefix_type)},
    });
  }
  const nlohmann::json json = {
      {"primaryKeyId", keyset->primary_id},
      {"key", keys},
  };
  return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// KEYS.KEYSET_FROM_JSON(json_keyset): a keyset in Tink's JSON format, serialized.
absl::StatusOr<std::string> KeysetFromJson(const Arguments& arguments) {
  const auto invalid = [] {
    return Error("KEYS.KEYSET_FROM_JSON", "Failed to parse JSON string as a Tink keyset-proto.",
                 " Error in ");
  };
  const auto json = nlohmann::json::parse(arguments.String(0), nullptr, false);
  if (!json.is_object()) {
    return invalid();
  }
  // A number of the JSON object, which protocol buffers take as 0 when it is missing.
  const auto number = [](const nlohmann::json& object, const char* name,
                         uint64_t max) -> std::optional<uint64_t> {
    if (!object.contains(name)) {
      return 0;
    }
    const nlohmann::json& value = object.at(name);
    if (!value.is_number_unsigned() || value.get<uint64_t>() > max) {
      return std::nullopt;
    }
    return value.get<uint64_t>();
  };
  // An enum of the JSON object, by name or value.
  const auto enumeration = [](const nlohmann::json& object, const char* name,
                              const auto& names) -> std::optional<uint64_t> {
    if (!object.contains(name)) {
      return 0;
    }
    return EnumValue(names, object.at(name));
  };
  Keyset keyset;
  const auto primary_id = number(json, "primaryKeyId", UINT32_MAX);
  if (!primary_id) {
    return invalid();
  }
  keyset.primary_id = static_cast<uint32_t>(*primary_id);
  const nlohmann::json keys = json.value("key", nlohmann::json::array());
  if (!keys.is_array()) {
    return invalid();
  }
  for (const nlohmann::json& key : keys) {
    if (!key.is_object()) {
      return invalid();
    }
    const nlohmann::json key_data = key.value("keyData", nlohmann::json::object());
    if (!key_data.is_object()) {
      return invalid();
    }
    const nlohmann::json type_url = key_data.value("typeUrl", "");
    const nlohmann::json value = key_data.value("value", "");

    const auto key_material_type = enumeration(key_data, "keyMaterialType", kKeyMaterialTypes);
    const auto status = enumeration(key, "status", kStatuses);
    const auto id = number(key, "keyId", UINT32_MAX);
    const auto output_prefix_type = enumeration(key, "outputPrefixType", kOutputPrefixTypes);
    if (!type_url.is_string() || !value.is_string() || !key_material_type || !status || !id ||
        !output_prefix_type) {
      return invalid();
    }
    keyset.keys.push_back({
        .type_url = type_url.get<std::string>(),
        .value = FromBase64(value.get<std::string>()),
        .key_material_type = *key_material_type,
        .status = *status,
        .id = static_cast<uint32_t>(*id),
        .output_prefix_type = *output_prefix_type,
    });
  }
  return Serialize(keyset);
}

absl::StatusOr<std::string> AeadDecryptBytes(const Arguments& arguments) {
  return Decrypt("AEAD.DECRYPT_BYTES", kAead, arguments);
}

absl::StatusOr<std::string> AeadDecryptString(const Arguments& arguments) {
  return DecryptString("AEAD.DECRYPT_STRING", "AEAD.DECRYPT_BYTES", kAead, arguments);
}

absl::StatusOr<std::string> DeterministicDecryptBytes(const Arguments& arguments) {
  return Decrypt("DETERMINISTIC_DECRYPT_BYTES", kDeterministicAead, arguments);
}

absl::StatusOr<std::string> DeterministicDecryptString(const Arguments& arguments) {
  return DecryptString("DETERMINISTIC_DECRYPT_STRING", "DETERMINISTIC_DECRYPT_BYTES",
                       kDeterministicAead, arguments);
}

}  // namespace

void RegisterAeadFunctions(duckdb_connection connection) {
  // A new random IV makes each call's ciphertext differ.
  Register(connection, "bq_aead_encrypt", {kBlob, kBlob, kBlob}, kBlob, EachRow<Encrypt>, true,
           std::nullopt, true);
  Register(connection, "bq_aead_decrypt_bytes", {kBlob, kBlob, kBlob}, kBlob,
           EachRow<AeadDecryptBytes>);
  Register(connection, "bq_aead_decrypt_string", {kBlob, kBlob, kBlob}, kVarchar,
           EachRow<AeadDecryptString>);
  Register(connection, "bq_deterministic_encrypt", {kBlob, kBlob, kBlob}, kBlob,
           EachRow<DeterministicEncrypt>);
  Register(connection, "bq_deterministic_decrypt_bytes", {kBlob, kBlob, kBlob}, kBlob,
           EachRow<DeterministicDecryptBytes>);
  Register(connection, "bq_deterministic_decrypt_string", {kBlob, kBlob, kBlob}, kVarchar,
           EachRow<DeterministicDecryptString>);
  // New random keys make each call's keyset differ. A NULL key type is an error.
  Register(connection, "bq_keys_new_keyset", {kVarchar}, kBlob, EachRow<NewKeyset, false>, false,
           std::nullopt, true);
  Register(connection, "bq_keys_rotate_keyset", {kBlob, kVarchar}, kBlob,
           EachRow<RotateKeyset, false>, false, std::nullopt, true);
  // GoogleSQL takes this as immutable, which lets SAFE. take it, although the key ID is random.
  Register(connection, "bq_keys_add_aes_gcm_key", {kBlob, kBlob}, kBlob, EachRow<AddAesGcmKey>);
  Register(connection, "bq_keys_keyset_length", {kBlob}, kBigint, EachRow<KeysetLength>);
  Register(connection, "bq_keys_keyset_to_json", {kBlob}, kVarchar, EachRow<KeysetToJson>);
  Register(connection, "bq_keys_keyset_from_json", {kVarchar}, kBlob, EachRow<KeysetFromJson>);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
