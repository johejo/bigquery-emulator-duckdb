"""Inspect header-check actions independently of clang-tidy."""

load(":heavy_includes.bzl", "heavy_includes")

_Actions = provider(fields = ["headers"])

def _inspect_impl(target, _ctx):
    return [_Actions(headers = [a for a in target.actions if a.mnemonic == "HeavyIncludes"])]

_inspect = aspect(
    implementation = _inspect_impl,
    requires = [heavy_includes],
)

def _heavy_includes_test_impl(ctx):
    if len(ctx.attr.header_target[_Actions].headers) != 1:
        fail("Headers must be checked for heavy GoogleSQL includes")
    if ctx.attr.source_target[_Actions].headers:
        fail("Sources must not be checked for heavy GoogleSQL includes")
    executable = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(executable, "#!/bin/sh\nexit 0\n", is_executable = True)
    return [DefaultInfo(executable = executable)]

heavy_includes_test = rule(
    implementation = _heavy_includes_test_impl,
    attrs = {
        "header_target": attr.label(mandatory = True, aspects = [_inspect]),
        "source_target": attr.label(mandatory = True, aspects = [_inspect]),
    },
    test = True,
)
