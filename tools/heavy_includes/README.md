# Heavy GoogleSQL includes

`just heavy-includes` or `nix run .#heavy-includes` checks repository headers using
the C++ toolchain's preprocessor, without clang-tidy. To check one target and its dependencies:

```sh
nix develop --command bazelisk build --config=heavy-includes //:translator
```

GoogleSQL's resolved AST, analyzer and script executor headers take about 30s each to parse at
-O0, and a header that includes one passes that time to every file that includes it, also when it
gets there through another GoogleSQL header such as `analyzer_options.h`. Each header is therefore
preprocessed with the stand-ins in `heavy_headers/` found first, whose `#error` makes clang
show the chain of includes that reached one.

The aspect checks checked-in headers in both `srcs` and `hdrs`, traversing `deps` and
`implementation_deps`. Generated and external files are omitted. Each header has its own
cached action using the target's compiler flags, defines, include paths and environment.
`just lint` and CI run this separately from clang-tidy.

`//:heavy_includes_test` checks that actions are created for headers only.
