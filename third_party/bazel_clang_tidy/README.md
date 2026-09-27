# bazel_clang_tidy patches

`cxx_include_order.patch` reorders the toolchain's builtin include directories that
the clang-tidy aspect passes as `-isystem`, so that the C++ standard library headers
come before the C ones.

Apple's toolchain reports them the other way around, and libc++ hard-errors
("tried including <ctype.h> but didn't find libc++'s <ctype.h> header") when a C
standard library directory is searched first. The upstream aspect passes them in the
order the toolchain reports, so clang-tidy cannot parse anything on macOS without this.

`apple_frameworks.patch`, applied on top, passes builtin framework directories such as
the macOS SDK's `System/Library/Frameworks` as `-iframework` instead of `-isystem`.
clang only looks up `<Framework/Header.h>` includes in framework paths, so without it
clang-tidy cannot parse anything that includes a system framework, such as cpp-httplib
with TLS, which reads root certificates from the Keychain.

Drop each patch once it is fixed upstream.
