#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace googlesql_aead {

absl::StatusOr<std::string> Encrypt(std::string_view keyset_bytes, std::string_view input,
                                    std::string_view aad);
absl::StatusOr<std::string> DeterministicEncrypt(std::string_view keyset_bytes,
                                                 std::string_view input, std::string_view aad);
absl::StatusOr<std::string> AeadDecryptBytes(std::string_view keyset_bytes, std::string_view input,
                                             std::string_view aad);
absl::StatusOr<std::string> AeadDecryptString(std::string_view keyset_bytes, std::string_view input,
                                              std::string_view aad);
absl::StatusOr<std::string> DeterministicDecryptBytes(std::string_view keyset_bytes,
                                                      std::string_view input, std::string_view aad);
absl::StatusOr<std::string> DeterministicDecryptString(std::string_view keyset_bytes,
                                                       std::string_view input,
                                                       std::string_view aad);
absl::StatusOr<std::string> NewKeyset(std::string_view key_type);
absl::StatusOr<std::optional<std::string>> RotateKeyset(
    std::optional<std::string_view> keyset_bytes, std::string_view key_type);
absl::StatusOr<std::string> AddAesGcmKey(std::string_view keyset_bytes, std::string_view raw);
absl::StatusOr<int64_t> KeysetLength(std::string_view keyset_bytes);
absl::StatusOr<std::string> KeysetToJson(std::string_view keyset_bytes);
absl::StatusOr<std::string> KeysetFromJson(std::string_view keyset_bytes);

}  // namespace googlesql_aead
