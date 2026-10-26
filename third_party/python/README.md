# Embedded CPython

The full python-build-standalone distributions pinned in `MODULE.bazel` supply `libpython.a`,
its static dependencies, headers, standard library and native modules together. `runtime` selects
the archive for the target platform; `python3` identifies its installation for worker initialization,
and `files` makes the standard library available in runfiles. Build tools still use rules_python's
separate interpreter.

The bundled `libssl.a` and `libcrypto.a` are excluded. CPython and the emulator's other dependencies
link the same Bazel OpenSSL instead, avoiding duplicate crypto symbols. The static CPython archive
contains LLVM bitcode; macOS builds use LLVM's Mach-O linker from the Nix toolchain rather than
Apple's older LTO reader. The executable exports its symbols for Python extension modules.
