# clang-tidy aspect

`nix develop --command just tidy` checks the repository with one cached action per source or
header. To check one target and its dependencies:

```sh
nix develop --command bazelisk build --config=clang-tidy //:translator
```

The tool is resolved by `repository.bzl` from `CLANG_TIDY` (set by the development shell), or
from PATH. Outside the shell, `--repo_env=CLANG_TIDY=/absolute/path/to/clang-tidy` overrides it.
Without either, the tool is a stub that fails each lint action, so builds and tests that only
analyze the aspect, such as `//:clang_tidy_test`, do not need clang-tidy.
The executable is a declared action input, so changing it invalidates cached results. This uses
the local development toolchain, including its SDK and immutable Nix store paths; it does not
provide a portable remote-execution toolchain.

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

clang-tidy discovers each file's nearest `.clang-tidy` and any inherited configuration. Add every
new configuration to `//:clang_tidy_config`, so sandboxed actions can read it. Each action includes
only the configurations in the source file's ancestor directories, so changes invalidate the
affected files' cache. `WarningsAsErrors: '*'` makes findings fail the action; failed actions cannot
produce the success marker.

`//:clang_tidy_test` inspects the real `query_parameters_test`, `query_parameters` and `httplib`
targets. It checks configuration and executable inputs, C++20 flags, httplib's public settings
and private-define isolation, and implementation dependencies' lint outputs. It is included in
`just test`; `just tidy` executes the actual lint actions.
