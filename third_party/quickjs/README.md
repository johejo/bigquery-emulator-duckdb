# QuickJS patches

`unicode_symbol.patch` namespaces QuickJS's `unicode_normalize` as
`quickjs_unicode_normalize` through its internal header, covering the definition and all
callers. PostgreSQL exports an incompatible C function named `unicode_normalize` too.

Keeping the symbols distinct avoids duplicate symbols or binding calls to the wrong
implementation when the emulator statically links both libraries, regardless of link order.

`throw_location.patch` records the source position of an explicit `throw` separately from
`Error.stack`. The parser saves the `throw` token's location in a dedicated opcode so that
compiler-generated rethrows do not get attributed to a source statement. The host can read the
location of a pending throw from the function it called without relying on JavaScript stack
formatting or changing the `Error` object.
