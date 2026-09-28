# bazel_clang_tidy patch

`cxx_include_order.patch` keeps the C++ standard library headers in front of the other
builtin include directories that the clang-tidy aspect passes to clang-tidy. All directories
remain `-isystem`, with their original order preserved within each group.

The upstream aspect passes all of them as `-isystem`, in the order the toolchain reports
them. Apple's toolchain reports the SDK's C headers ahead of libc++, which libc++
hard-errors on ("tried including <ctype.h> but didn't find libc++'s <ctype.h> header"),
so clang-tidy cannot parse anything on macOS without this.

Using `-idirafter` for the C directories changes their priority relative to implicit
include paths. With Nix's clang-tidy wrapper and Apple's toolchain, this mixes Nix
and Apple headers and breaks standard library declarations. Keep the change limited
to reordering the explicit `-isystem` paths. The native Linux case reported in
https://github.com/erenon/bazel_clang_tidy/issues/106 has not been verified here.

Drop the patch once it is fixed upstream.
