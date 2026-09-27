# cpp-httplib

Wraps `@cpp-httplib//:httplib` with `CPPHTTPLIB_OPENSSL_SUPPORT` defined so HTTPS works through
the BoringSSL that the cpp-httplib module already depends on. Depend on
`//third_party/cpp_httplib:httplib` rather than the upstream target: `httplib.h` changes its class
layouts with that define, and mixing the two in one binary would break the one-definition rule.

`CPPHTTPLIB_DISABLE_MACOSX_AUTOMATIC_ROOT_CERTIFICATES` keeps httplib from reading root
certificates from the macOS Keychain. It verifies servers against BoringSSL's default CA paths
(`/etc/ssl/cert.pem` and `/etc/ssl/certs`) on every platform instead, and `httplib.h` includes
no Apple framework headers, which clang-tidy could not find and whose macros collide with
GoogleSQL's.
