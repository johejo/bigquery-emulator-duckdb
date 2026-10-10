"""Expose the development shell's clang-tidy as a declared Bazel input."""

_MISSING = """\
#!/bin/sh
echo "clang-tidy was not found; run \\`nix develop --command just tidy\\` or set --repo_env=CLANG_TIDY=/absolute/path/to/clang-tidy" >&2
exit 1
"""

def _clang_tidy_repository_impl(ctx):
    ctx.file("BUILD.bazel", 'exports_files(["clang-tidy"], visibility = ["//visibility:public"])\n')
    executable = ctx.os.environ.get("CLANG_TIDY") or ctx.which("clang-tidy")
    if not executable:
        # Fail only lint actions, so builds and tests that merely analyze the aspect still work.
        ctx.file("clang-tidy", _MISSING, executable = True)
        return
    if not str(executable).startswith("/"):
        fail("CLANG_TIDY must name an existing absolute executable path")
    executable = ctx.path(executable)
    if not executable.exists:
        fail("CLANG_TIDY must name an existing absolute executable path")
    ctx.symlink(executable, "clang-tidy")

clang_tidy_repository = repository_rule(
    implementation = _clang_tidy_repository_impl,
    configure = True,
    local = True,
    environ = ["CLANG_TIDY", "PATH"],
)
