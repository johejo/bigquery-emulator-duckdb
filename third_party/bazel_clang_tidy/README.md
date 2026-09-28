# bazel_clang_tidy patch

`cxx_include_order.patch` keeps the C++ standard library headers in front of the other
builtin include directories that the clang-tidy aspect passes to clang-tidy. It passes the
C++ ones as `-isystem` and the others as `-idirafter`.

The upstream aspect passes all of them as `-isystem`, in the order the toolchain reports
them. Apple's toolchain reports the SDK's C headers ahead of libc++, which libc++
hard-errors on ("tried including <ctype.h> but didn't find libc++'s <ctype.h> header"),
so clang-tidy cannot parse anything on macOS without this. The same ordering breaks
libstdc++'s `#include_next <stdlib.h>` on native Linux toolchains; see
https://github.com/erenon/bazel_clang_tidy/issues/106.

Drop the patch once it is fixed upstream.
