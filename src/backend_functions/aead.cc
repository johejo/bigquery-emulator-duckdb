#include <openssl/aead.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "utf8_validity.h"

// AEAD.ENCRYPT, AEAD.DECRYPT_BYTES and AEAD.DECRYPT_STRING, which take a serialized Tink keyset
// of AES-GCM keys and produce and read Tink's ciphertexts: the output prefix of the key, a
// 12-byte IV, and the encrypted data followed by a 16-byte tag.

#include "src/backend_functions/aead.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

constexpr std::string_view kAesGcmKey = "type.googleapis.com/google.crypto.tink.AesGcmKey";
constexpr std::size_t kIvSize = 12;
constexpr std::size_t kTagSize = 16;

// Tink's KeyStatusType and OutputPrefixType.
constexpr uint64_t kEnabled = 1;
constexpr uint64_t kTink = 1;
constexpr uint64_t kLegacy = 2;
constexpr uint64_t kRaw = 3;
constexpr uint64_t kCrunchy = 4;

// A field of a serialized protocol buffer message, with either `varint` or `bytes`.
struct Field {
  uint64_t number = 0;
  uint64_t varint = 0;
  std::string_view bytes = {};
};

bool ReadVarint(std::string_view& data, uint64_t& value) {
  value = 0;
  for (int shift = 0; shift < 64 && !data.empty(); shift += 7) {
    const auto byte = static_cast<uint8_t>(data.front());
    data.remove_prefix(1);
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      return true;
    }
  }
  return false;
}

absl::Status Malformed() { return absl::OutOfRangeError("Could not parse keyset"); }

// The fields of a serialized message, skipping fixed-size ones.
absl::StatusOr<std::vector<Field>> ParseMessage(std::string_view data) {
  std::vector<Field> fields;
  while (!data.empty()) {
    uint64_t tag = 0;
    if (!ReadVarint(data, tag)) {
      return Malformed();
    }
    Field field{.number = tag >> 3};
    uint64_t size = 0;
    switch (tag & 7) {
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

// An enabled key of a keyset.
struct Key {
  uint32_t id = 0;
  // The ciphertexts of the key start with this; empty for RAW keys.
  std::string prefix;
  const EVP_AEAD* aead = nullptr;
  std::string value;
};

struct Keyset {
  std::vector<Key> keys;
  // The key that encrypts.
  Key primary;
};

// The AES key of a serialized AesGcmKey: its version, then the key.
absl::StatusOr<std::string> AesGcmKeyValue(std::string_view data) {
  const auto fields = ParseMessage(data);
  if (!fields.ok()) {
    return fields.status();
  }
  uint64_t version = 0;
  std::string value;
  for (const Field& field : *fields) {
    if (field.number == 1) {
      version = field.varint;
    } else if (field.number == 3) {
      value = field.bytes;
    }
  }
  if (version != 0) {
    return absl::OutOfRangeError("AesGcmKey has an unsupported version");
  }
  if (value.size() != 16 && value.size() != 32) {
    return absl::OutOfRangeError("AesGcmKey has an invalid key size");
  }
  return value;
}

// A serialized Keyset.Key, or nullopt when it is not enabled: its KeyData, status, ID and output
// prefix type.
absl::StatusOr<std::optional<Key>> ParseKey(std::string_view data) {
  const auto fields = ParseMessage(data);
  if (!fields.ok()) {
    return fields.status();
  }
  std::string_view key_data;
  uint64_t status = 0;
  uint64_t prefix_type = 0;
  Key key;
  for (const Field& field : *fields) {
    if (field.number == 1) {
      key_data = field.bytes;
    } else if (field.number == 2) {
      status = field.varint;
    } else if (field.number == 3) {
      key.id = static_cast<uint32_t>(field.varint);
    } else if (field.number == 4) {
      prefix_type = field.varint;
    }
  }
  if (status != kEnabled) {
    return std::nullopt;
  }
  switch (prefix_type) {
    case kTink:
    case kLegacy:
    case kCrunchy:
      key.prefix.push_back(prefix_type == kTink ? '\x01' : '\x00');
      for (int shift = 24; shift >= 0; shift -= 8) {
        key.prefix.push_back(static_cast<char>((key.id >> shift) & 0xff));
      }
      break;
    case kRaw:
      break;
    default:
      return absl::OutOfRangeError("Key has an unknown output prefix type");
  }
  // KeyData: the type URL, then the serialized key.
  const auto key_fields = ParseMessage(key_data);
  if (!key_fields.ok()) {
    return key_fields.status();
  }
  std::string_view type_url;
  std::string_view value;
  for (const Field& field : *key_fields) {
    if (field.number == 1) {
      type_url = field.bytes;
    } else if (field.number == 2) {
      value = field.bytes;
    }
  }
  if (type_url != kAesGcmKey) {
    return absl::OutOfRangeError("Key type is unsupported: " + std::string(type_url));
  }
  auto aes = AesGcmKeyValue(value);
  if (!aes.ok()) {
    return aes.status();
  }
  key.value = *std::move(aes);
  key.aead = key.value.size() == 16 ? EVP_aead_aes_128_gcm() : EVP_aead_aes_256_gcm();
  return key;
}

// A serialized Tink Keyset: the ID of its primary key, then its keys.
absl::StatusOr<Keyset> ParseKeyset(std::string_view data) {
  const auto fields = ParseMessage(data);
  if (!fields.ok()) {
    return fields.status();
  }
  uint64_t primary_id = 0;
  bool any = false;
  Keyset keyset;
  for (const Field& field : *fields) {
    if (field.number == 1) {
      primary_id = field.varint;
    } else if (field.number == 2) {
      any = true;
      auto key = ParseKey(field.bytes);
      if (!key.ok()) {
        return key.status();
      }
      if (*key) {
        keyset.keys.push_back(**std::move(key));
      }
    }
  }
  if (!any) {
    return absl::OutOfRangeError("A valid keyset must contain at least one key.");
  }
  const auto primary = std::ranges::find_if(
      keyset.keys, [primary_id](const Key& key) { return key.id == primary_id; });
  if (primary == keyset.keys.end()) {
    return absl::OutOfRangeError("Keyset has no enabled primary key");
  }
  keyset.primary = *primary;
  return keyset;
}

// Seals or opens `in` with `key`, or returns nullopt when opening fails.
std::optional<std::string> Seal(const Key& key, std::string_view iv, std::string_view in,
                                std::string_view aad, bool open) {
  bssl::ScopedEVP_AEAD_CTX context;
  if (EVP_AEAD_CTX_init(context.get(), key.aead, reinterpret_cast<const uint8_t*>(key.value.data()),
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

absl::Status Failed(std::string_view function, std::string_view message) {
  return absl::OutOfRangeError(std::string(function) + " failed: " + std::string(message));
}

// The keyset of the first argument.
absl::StatusOr<Keyset> KeysetOf(std::string_view function, const Arguments& arguments) {
  auto keyset = ParseKeyset(arguments.String(0));
  if (!keyset.ok()) {
    return Failed(function,
                  "Creation of AEAD primitive failed: " + std::string(keyset.status().message()));
  }
  return keyset;
}

// AEAD.ENCRYPT(keyset, plaintext, additional_data).
absl::StatusOr<std::string> Encrypt(const Arguments& arguments) {
  constexpr std::string_view kFunction = "AEAD.ENCRYPT";
  const auto keyset = KeysetOf(kFunction, arguments);
  if (!keyset.ok()) {
    return keyset.status();
  }
  std::string iv(kIvSize, '\0');
  RAND_bytes(reinterpret_cast<uint8_t*>(iv.data()), iv.size());
  const auto sealed = Seal(keyset->primary, iv, arguments.String(1), arguments.String(2), false);
  if (!sealed) {
    return Failed(kFunction, "encryption failed");
  }
  return keyset->primary.prefix + iv + *sealed;
}

// AEAD.DECRYPT_*(keyset, ciphertext, additional_data). Tries the keys whose prefix the
// ciphertext starts with, then the RAW keys, as Tink does.
absl::StatusOr<std::string> Decrypt(std::string_view function, const Arguments& arguments) {
  const auto keyset = KeysetOf(function, arguments);
  if (!keyset.ok()) {
    return keyset.status();
  }
  const std::string ciphertext = arguments.String(1);
  const std::string aad = arguments.String(2);
  for (const bool raw : {false, true}) {
    for (const Key& key : keyset->keys) {
      if (key.prefix.empty() != raw || !ciphertext.starts_with(key.prefix)) {
        continue;
      }
      const std::string_view rest = std::string_view(ciphertext).substr(key.prefix.size());
      if (rest.size() < kIvSize + kTagSize) {
        continue;
      }
      if (auto plaintext = Seal(key, rest.substr(0, kIvSize), rest.substr(kIvSize), aad, true)) {
        return *std::move(plaintext);
      }
    }
  }
  return Failed(function, "decryption failed");
}

void AeadEncrypt(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, Encrypt);
}

void AeadDecryptBytes(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) { return Decrypt("AEAD.DECRYPT_BYTES", arguments); });
}

void AeadDecryptString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    constexpr std::string_view kFunction = "AEAD.DECRYPT_STRING";
    auto plaintext = Decrypt(kFunction, arguments);
    if (plaintext.ok() && !utf8_range::IsStructurallyValid(*plaintext)) {
      return Failed(kFunction,
                    "Decrypted plaintext is not a valid UTF-8 string. To decrypt to BYTES, use "
                    "AEAD.DECRYPT_BYTES");
    }
    return plaintext;
  });
}

}  // namespace

void RegisterAeadFunctions(duckdb_connection connection) {
  // A new random IV makes each call's ciphertext differ.
  Register(connection, "bq_aead_encrypt", {kBlob, kBlob, kBlob}, kBlob, AeadEncrypt, true,
           std::nullopt, true);
  Register(connection, "bq_aead_decrypt_bytes", {kBlob, kBlob, kBlob}, kBlob, AeadDecryptBytes);
  Register(connection, "bq_aead_decrypt_string", {kBlob, kBlob, kBlob}, kVarchar,
           AeadDecryptString);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
