"""Inspect lint actions for the repository's real targets; just tidy executes them."""

load(":clang_tidy.bzl", "clang_tidy")

_Actions = provider(fields = ["actions", "implementation_outputs"])

def _inspect_impl(target, ctx):
    return [_Actions(
        actions = [a for a in target.actions if a.mnemonic == "ClangTidy"],
        implementation_outputs = depset(transitive = [
            dep[OutputGroupInfo].clang_tidy
            for dep in getattr(ctx.rule.attr, "implementation_deps", [])
            if OutputGroupInfo in dep and hasattr(dep[OutputGroupInfo], "clang_tidy")
        ]),
    )]

_inspect = aspect(
    implementation = _inspect_impl,
    requires = [clang_tidy],
)

def _clang_tidy_test_impl(ctx):
    for target in (ctx.attr.test_target, ctx.attr.header_target):
        actions = target[_Actions].actions
        if not actions:
            fail("No lint actions for " + str(target.label))
        for action in actions:
            configs = sorted([f.short_path for f in action.inputs.to_list() if f.basename == ".clang-tidy"])
            expected = [".clang-tidy", "tests/.clang-tidy"] if target == ctx.attr.test_target else [".clang-tidy"]
            if configs != expected:
                fail("Wrong configurations for {}: {}".format(target.label, configs))
            if not any([f.basename == "clang-tidy" for f in action.inputs.to_list()]):
                fail("clang-tidy executable is not an action input")
    target = ctx.attr.implementation_target
    expected = target[_Actions].implementation_outputs.to_list()
    if not expected:
        fail("No implementation dependency lint outputs for " + str(target.label))
    outputs = target[OutputGroupInfo].clang_tidy.to_list()
    if any([f not in outputs for f in expected]):
        fail("Missing implementation dependency lint outputs for " + str(target.label))
    if any([f.owner.workspace_name for f in outputs]):
        fail("External targets must not produce lint outputs")
    executable = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(executable, "#!/bin/sh\nexit 0\n", is_executable = True)
    return [DefaultInfo(executable = executable)]

clang_tidy_test = rule(
    implementation = _clang_tidy_test_impl,
    attrs = {
        "test_target": attr.label(mandatory = True, aspects = [_inspect]),
        "header_target": attr.label(mandatory = True, aspects = [_inspect]),
        "implementation_target": attr.label(mandatory = True, aspects = [_inspect]),
    },
    test = True,
)
