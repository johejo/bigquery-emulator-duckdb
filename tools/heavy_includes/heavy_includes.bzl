"""Reject expensive GoogleSQL includes in repository headers using the C++ compiler."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cpp_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//tools/cc_checks:compile_flags.bzl", "compile_flags")

def _heavy_includes_impl(target, ctx):
    if CcInfo not in target or target.label.workspace_name:
        return []

    inherited = [
        dep[OutputGroupInfo].heavy_includes
        for attr in ("deps", "implementation_deps")
        for dep in getattr(ctx.rule.attr, attr, [])
        if OutputGroupInfo in dep and hasattr(dep[OutputGroupInfo], "heavy_includes")
    ]
    srcs = [
        f
        for attr in ("srcs", "hdrs")
        for t in getattr(ctx.rule.attr, attr, [])
        for f in t.files.to_list()
        if f.is_source and f.extension == "h"
    ]
    if not srcs:
        return [OutputGroupInfo(heavy_includes = depset(transitive = inherited))]

    compilation_context = cc_common.merge_compilation_contexts(compilation_contexts = [
        dep[CcInfo].compilation_context
        for dep in [target] + getattr(ctx.rule.attr, "implementation_deps", [])
    ])
    cc_toolchain = find_cpp_toolchain(ctx)
    flags, env = compile_flags(
        ctx,
        cc_toolchain,
        compilation_context,
        target[CcInfo].compilation_context.local_defines,
    )

    inputs = depset(
        transitive = [compilation_context.headers, cc_toolchain.all_files],
    )
    outputs = [
        _check_heavy_includes(ctx, cc_toolchain, src, flags, env, inputs)
        for src in srcs
    ]
    return [OutputGroupInfo(heavy_includes = depset(outputs, transitive = inherited))]

def _check_heavy_includes(ctx, cc_toolchain, header, flags, env, inputs):
    out = ctx.actions.declare_file("{}.heavy-includes/{}".format(ctx.label.name, header.short_path))
    stand_ins = ctx.files._heavy_headers
    root = stand_ins[0].path[:stand_ins[0].path.index("/googlesql/")]
    args = ctx.actions.args()
    args.add(cc_toolchain.compiler_executable)
    args.add(out)

    # Quoted includes search -iquote directories in order, so the stand-ins come first.
    args.add_all(["-iquote", root])
    args.add_all(flags)
    args.add_all(["-Wno-pragma-once-outside-header", "-E", "-o", "/dev/null", header])
    ctx.actions.run_shell(
        command = 'tool=$1; out=$2; shift 2; "$tool" "$@" && touch "$out"',
        arguments = [args],
        inputs = depset([header] + stand_ins, transitive = [inputs]),
        outputs = [out],
        mnemonic = "HeavyIncludes",
        progress_message = "Check the GoogleSQL headers " + header.short_path + " includes",
        use_default_shell_env = True,
        env = env,
    )
    return out

heavy_includes = aspect(
    implementation = _heavy_includes_impl,
    attr_aspects = ["deps", "implementation_deps"],
    attrs = {
        "_heavy_headers": attr.label(default = Label("//tools/heavy_includes:heavy_headers")),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
