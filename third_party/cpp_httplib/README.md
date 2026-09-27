# cpp-httplib

Wraps `@cpp-httplib//:httplib` with `CPPHTTPLIB_OPENSSL_SUPPORT` defined so HTTPS works through
the BoringSSL that the cpp-httplib module already depends on. Depend on
`//third_party/cpp_httplib:httplib` rather than the upstream target: `httplib.h` changes its class
layouts with that define, and mixing the two in one binary would break the one-definition rule.
