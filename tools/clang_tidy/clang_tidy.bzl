"""Runs clang-tidy on every C++ source and header of the repository's targets.

Each checked-in file is its own action, so results are cached and checked in parallel. Generated
and external files are omitted. Headers are checked as their own translation units too, since
checks such as misc-include-cleaner look only at the main file.
"""

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cpp_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//tools/cc_checks:compile_flags.bzl", "compile_flags")

def _clang_tidy_impl(target, ctx):
    if CcInfo not in target or target.label.workspace_name:
        return []

    inherited = [
        dep[OutputGroupInfo].clang_tidy
        for attr in ("deps", "implementation_deps")
        for dep in getattr(ctx.rule.attr, attr, [])
        if OutputGroupInfo in dep and hasattr(dep[OutputGroupInfo], "clang_tidy")
    ]
    srcs = [
        f
        for attr in ("srcs", "hdrs")
        for t in getattr(ctx.rule.attr, attr, [])
        for f in t.files.to_list()
        if f.is_source and f.extension in ("cc", "h")
    ]
    if not srcs:
        return [OutputGroupInfo(clang_tidy = depset(transitive = inherited))]

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
        builtin_includes = True,
    )

    inputs = depset(
        transitive = [compilation_context.headers, cc_toolchain.all_files],
    )
    outputs = []
    for src in srcs:
        # Let clang-tidy discover its configuration as outside Bazel. Only ancestor configs can
        # apply, so changes to tests/.clang-tidy do not invalidate production sources.
        configs = [
            config
            for config in ctx.files._configs
            if src.path.startswith(config.path[:-len(".clang-tidy")])
        ]
        out = ctx.actions.declare_file("{}.clang-tidy/{}".format(ctx.label.name, src.short_path))
        args = ctx.actions.args()
        args.add(ctx.file._clang_tidy)
        args.add(out)
        args.add_all(["--quiet", src, "--"])
        args.add_all(flags)
        ctx.actions.run_shell(
            command = 'tool=$1; out=$2; shift 2; "$tool" "$@" && touch "$out"',
            arguments = [args],
            tools = [ctx.file._clang_tidy],
            inputs = depset([src] + configs, transitive = [inputs]),
            outputs = [out],
            mnemonic = "ClangTidy",
            progress_message = "Run clang-tidy on " + src.short_path,
            # Nix wrappers and the C++ toolchain use the development shell's environment.
            use_default_shell_env = True,
            env = env,
        )
        outputs.append(out)
    return [OutputGroupInfo(clang_tidy = depset(outputs, transitive = inherited))]

clang_tidy = aspect(
    implementation = _clang_tidy_impl,
    attr_aspects = ["deps", "implementation_deps"],
    attrs = {
        "_clang_tidy": attr.label(default = Label("@clang_tidy_tool//:clang-tidy"), allow_single_file = True, cfg = "exec"),
        # Lists every .clang-tidy in the repository.
        "_configs": attr.label(default = Label("//:clang_tidy_config")),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
