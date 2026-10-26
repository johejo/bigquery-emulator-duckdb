# OpenSSL

`openssl.patch` changes the Bazel dependencies of the REST and Storage clients from BoringSSL
to OpenSSL. This keeps them on the same crypto implementation as gRPC, curl, libpq, MariaDB
and the embedded CPython runtime. The code already supports OpenSSL; no source changes are needed.
