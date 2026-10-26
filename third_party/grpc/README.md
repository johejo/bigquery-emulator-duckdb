# OpenSSL headers

`openssl.patch` adds a direct crypto dependency to the TLS session cache. The OpenSSL Bazel
module exports its public `openssl/ssl.h` from `:crypto`, not `:ssl`, so this is required for
Bazel's header dependency checks when `grpc_use_openssl` is enabled. Other TLS targets already
depend on both libraries.
