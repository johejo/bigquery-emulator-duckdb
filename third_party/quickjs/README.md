# QuickJS Unicode symbol

`unicode_symbol.patch` namespaces QuickJS's `unicode_normalize` as
`quickjs_unicode_normalize` through its internal header, covering the definition and all
callers. PostgreSQL exports an incompatible C function named `unicode_normalize` too.

Keeping the symbols distinct avoids duplicate symbols or binding calls to the wrong
implementation when the emulator statically links both libraries, regardless of link order.
