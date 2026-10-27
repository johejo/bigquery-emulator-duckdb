# Embedded CPython

The full python-build-standalone distributions pinned in `MODULE.bazel` supply `libpython.a`,
its static dependencies, headers, standard library and native modules together. `runtime` selects
the archive for the target platform; `python3` identifies its installation for worker initialization,
and `files` makes the standard library available in runfiles. Build tools still use rules_python's
separate interpreter.

The bundled `libssl.a` and `libcrypto.a` are excluded. CPython and the emulator's other dependencies
link the same Bazel OpenSSL instead, avoiding duplicate crypto symbols. The static CPython archive
contains LLVM bitcode. `BUILD.python.bazel` selects `native_archive_linux.bzl` or
`native_archive_macos.bzl` to compile it to native code with LLVM lld before any executable links
it. Bazel caches this action from the input archive, C++ toolchain and conversion options, so
relinking the emulator or tests does not repeat CPython's optimization and code
generation. The whole input archive is loaded, leaving external references for the final link
to resolve against the shared Bazel dependencies above.

Linux uses a relocatable full-LTO link. Mach-O lld does not support relocatable links, so macOS
uses a temporary dylib link with `-all_load`, `-undefined dynamic_lookup` and `-object_path_lto`.
Only the saved native LTO objects are archived; the dylib is not linked into the emulator or
needed at runtime. All members of the pinned macOS CPython archive are bitcode, so these objects
cover the whole archive. The conversion uses the C++ toolchain's linker flags to find Nix's LLVM
Mach-O linker rather than Apple's older LTO reader. The executable exports its symbols for
Python extension modules.
