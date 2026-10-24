#include "src/googlesql_aead_duckdb/aead.h"

#include <optional>
#include <string>
#include <string_view>

#include "duckdb.h"
#include "src/backend_functions/apply.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/googlesql_aead/aead.h"

namespace bigquery_emulator_duckdb::aead_duckdb {
namespace {
using namespace backend_functions;

auto NewKeyset(const Arguments& arguments) {
  return googlesql_aead::NewKeyset(arguments.IsNull(0) ? "" : arguments.String(0));
}

auto RotateKeyset(const Arguments& arguments) {
  const std::optional<std::string> keyset =
      arguments.IsNull(0) ? std::nullopt : std::optional(arguments.String(0));
  return googlesql_aead::RotateKeyset(
      keyset ? std::optional<std::string_view>(*keyset) : std::nullopt,
      arguments.IsNull(1) ? "" : arguments.String(1));
}

}  // namespace

void RegisterAeadFunctions(duckdb_connection connection) {
  // A new random IV makes each call's ciphertext differ.
  Register(connection, "bq_aead_encrypt", {kBlob, kBlob, kBlob}, kBlob,
           Apply<googlesql_aead::Encrypt>, true, std::nullopt, true);
  Register(connection, "bq_aead_decrypt_bytes", {kBlob, kBlob, kBlob}, kBlob,
           Apply<googlesql_aead::AeadDecryptBytes>);
  Register(connection, "bq_aead_decrypt_string", {kBlob, kBlob, kBlob}, kVarchar,
           Apply<googlesql_aead::AeadDecryptString>);
  Register(connection, "bq_deterministic_encrypt", {kBlob, kBlob, kBlob}, kBlob,
           Apply<googlesql_aead::DeterministicEncrypt>);
  Register(connection, "bq_deterministic_decrypt_bytes", {kBlob, kBlob, kBlob}, kBlob,
           Apply<googlesql_aead::DeterministicDecryptBytes>);
  Register(connection, "bq_deterministic_decrypt_string", {kBlob, kBlob, kBlob}, kVarchar,
           Apply<googlesql_aead::DeterministicDecryptString>);
  // New random keys make each call's keyset differ. A NULL key type is an error.
  Register(connection, "bq_keys_new_keyset", {kVarchar}, kBlob, EachRow<NewKeyset, false>, false,
           std::nullopt, true);
  Register(connection, "bq_keys_rotate_keyset", {kBlob, kVarchar}, kBlob,
           EachRow<RotateKeyset, false>, false, std::nullopt, true);
  // GoogleSQL takes this as immutable, which lets SAFE. take it, although the key ID is random.
  Register(connection, "bq_keys_add_aes_gcm_key", {kBlob, kBlob}, kBlob,
           Apply<googlesql_aead::AddAesGcmKey>);
  Register(connection, "bq_keys_keyset_length", {kBlob}, kBigint,
           Apply<googlesql_aead::KeysetLength>);
  Register(connection, "bq_keys_keyset_to_json", {kBlob}, kVarchar,
           Apply<googlesql_aead::KeysetToJson>);
  Register(connection, "bq_keys_keyset_from_json", {kVarchar}, kBlob,
           Apply<googlesql_aead::KeysetFromJson>);
}

}  // namespace bigquery_emulator_duckdb::aead_duckdb
