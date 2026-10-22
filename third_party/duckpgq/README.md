# DuckPGQ

The extension is pinned in MODULE.bazel and built statically against the same
DuckDB headers as the backend. The grammar genrule follows the extension's
CMake grammar embedding. Compile it as C++17, as upstream does: C++20 can
constant-initialize an inline static string in a read-only section, while
DuckDB's prebuilt C++17 parser attempts to initialize the same symbol at runtime.

`drop_name.patch` preserves the qualified name when DuckDB copies a DROP
statement. Without it, DROP PROPERTY GRAPH attempts to drop an empty name.
The Go client graph test covers creation, querying and dropping the graph.
