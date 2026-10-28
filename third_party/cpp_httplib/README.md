# cpp-httplib

`BUILD.cpp_httplib.bazel` builds the header from the release pinned in `MODULE.bazel` rather than
the Bazel Central Registry's module, which lags upstream releases and depends on BoringSSL and zlib.
The emulator serves plain HTTP without compression, so the library needs neither.
