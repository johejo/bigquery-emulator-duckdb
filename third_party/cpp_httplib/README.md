# OpenSSL

`openssl.patch` switches cpp-httplib's Bazel dependency from BoringSSL to OpenSSL, matching the
emulator's other TLS users and embedded CPython. Both `:ssl` and `:crypto` are direct dependencies
because OpenSSL's Bazel module exports the public SSL headers from `:crypto`.
