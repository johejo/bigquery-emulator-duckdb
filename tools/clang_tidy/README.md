# clang-tidy aspect

`nix run .#clang-tidy` checks the repository with one cached action per source or
header. To check one target and its dependencies:

```sh
nix develop --command bazelisk build --config=clang-tidy //:translator
```

## Tool resolution

The tool is resolved by `repository.bzl` from `CLANG_TIDY` (set by the app and development
shell), or from PATH. Outside the shell, `--repo_env=CLANG_TIDY=/absolute/path/to/clang-tidy` overrides it.
Without either, the tool is a stub that fails each lint action, so builds and tests that only
analyze the aspect, such as `//:clang_tidy_test`, do not need clang-tidy.
The executable is a declared action input, so changing it invalidates cached results. This uses
the local development toolchain, including its SDK and immutable Nix store paths; it does not
provide a portable remote-execution toolchain.

## Files and compiler flags

The aspect checks the repository's checked-in `.cc` and `.h` files, including headers as standalone
translation units. Generated and external files are not linted, but dependency headers and the
C++ toolchain's files are inputs. The target's `copts`, local defines, transitive defines,
include paths and compilation environment are passed to clang-tidy. Module and layering checks
remain the regular compiler's job.
This aspect is scoped to the BUILD definitions used here, whose `copts` are literal flags.
It does not expand or tokenize options. Source-specific `--per_file_copt` is not reproduced;
put flags that affect parsing in the target's `copts` or global `--copt`/`--cxxopt` options.

Builtin include directories stay `-isystem`, with C++ directories placed ahead of C directories
and the order preserved within each group. Apple's SDK reports C headers before libc++, which
breaks libc++'s `#include_next`. Using `-idirafter` instead mixes Nix and Apple headers.

## Configuration and caching

clang-tidy discovers each file's nearest `.clang-tidy` and any inherited configuration. Add every
new configuration to `//:clang_tidy_config`, so sandboxed actions can read it. Each action includes
only the configurations in the source file's ancestor directories, so changes invalidate the
affected files' cache. `WarningsAsErrors: '*'` makes findings fail the action; failed actions cannot
produce a successful output.

Each successful action records the headers read by clang-tidy's own preprocessor, including
system headers, using `-Wp,-MD`. This adds no second preprocessing pass. The output is Bazel's
`unused_inputs_list`: compilation headers that were available but not read. After the first run,
Bazel can reuse the result when only those headers change. The source and configurations remain
inputs, and changes to include paths, defines, candidate headers or the tool still invalidate the
action. All candidate headers are available whenever the action executes, so newly added includes
are checked normally. Dependency generation or parsing failures fail the action too.

This primarily improves incremental builds with Bazel's local action cache. The disk cache saved
by CI does not carry that local dependency-discovery state to a fresh Bazel output base; do not
expect the same reduction in cache misses between CI machines.

## Tests

`//:clang_tidy_test` inspects the real `parquet_metadata_test`, `query_parameters` and `httplib`
targets. It checks configuration and executable inputs, collection of implementation dependencies'
lint outputs, and exclusion of external targets. These checks protect caching and lint coverage
without fixing individual compiler flags or dependency filenames. It is included in `just test`;
`just tidy` executes the actual lint actions with the targets' parsing settings.
`//tools/clang_tidy:runner_test` checks dependency-file escaping and rejects missing dependency
rules, which must never produce a cached success.
