# Embedded CPython

The full python-build-standalone distributions pinned in `MODULE.bazel` supply `libpython.a`,
its static dependencies, headers, standard library and native modules together. `runtime` selects
the archive for the target platform; `python3` identifies its installation for worker initialization,
and `files` makes the standard library available in runfiles. Build tools still use rules_python's
separate interpreter.

The bundled `libssl.a` and `libcrypto.a` are excluded. CPython and the emulator's other dependencies
link the same Bazel OpenSSL instead, avoiding duplicate crypto symbols. The static CPython archive
contains LLVM bitcode. On Linux, `native_archive.bzl` runs a relocatable full-LTO link with LLVM
lld and archives the native object before any executable links it. Bazel caches this action from
the input archive, C++ toolchain and conversion options, so relinking the emulator or tests does
not repeat CPython's optimization and code generation. The whole input archive is retained, and
the relocatable link leaves external references for the final link to resolve against the shared
Bazel dependencies above. macOS builds still link the original bitcode with LLVM's Mach-O linker
from the Nix toolchain rather than Apple's older LTO reader. The executable exports its symbols
for Python extension modules.
