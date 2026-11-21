# QuickJS-NG

`MODULE.bazel` pins the upstream release archive until the release is available in the Bazel
Central Registry. `BUILD.quickjs.bazel` uses the upstream `qjsc` at build time to regenerate
the engine's built-in bytecode with the patched opcode table. The emulator links only the core
engine, not the CLI or the host I/O library.

## Local patches

`unicode_symbol.patch` namespaces QuickJS's `unicode_normalize` as
`quickjs_unicode_normalize` through its internal header, covering the definition and all
callers. PostgreSQL exports an incompatible C function named `unicode_normalize` too.

Keeping the symbols distinct avoids duplicate symbols or binding calls to the wrong
implementation when the emulator statically links both libraries, regardless of link order.

`throw_location.patch` records the source position of an explicit `throw` separately from
`Error.stack`. The parser saves the `throw` token's location in a dedicated opcode. A rethrow
after `finally` restores that location, even if the `finally` block handled another exception.
The host can read the location of a pending throw from the same source as the function it
called, including its nested functions, without relying on JavaScript stack formatting.
