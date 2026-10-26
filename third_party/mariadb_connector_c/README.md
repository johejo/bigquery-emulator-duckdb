# mariadb_connector_c

[MariaDB Connector/C](https://github.com/mariadb-corporation/mariadb-connector-c), pinned at
v3.4.8, supplies the MySQL protocol client used by duckdb-mysql and EXTERNAL_QUERY analysis.
It is licensed under [LGPL 2.1 or later](https://github.com/mariadb-corporation/mariadb-connector-c/blob/v3.4.8/COPYING.LIB).

The native Bazel target builds the POSIX socket transport and statically registers native,
SHA256 and caching SHA2 authentication. The checked-in configuration headers describe the
little-endian LP64 platforms supported by this repository; local infile loading is disabled.
The version header expands upstream's v3.4.8 template. TLS uses the same OpenSSL as
gRPC, libpq and the embedded CPython runtime.
Compression uses the existing zlib dependency. No client plugins need to be installed.
