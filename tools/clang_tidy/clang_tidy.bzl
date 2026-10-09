"""Runs clang-tidy on every C++ source and header of the repository's targets.

Each file is its own action, so results are cached and checked in parallel, with the flags of the
toolchain that builds it. Headers are checked as their own translation units too, since checks
such as misc-include-cleaner look only at the main file.
"""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cpp_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _compile_flags(ctx, cc_toolchain, compilation_context, local_defines):
    feature_configuration = cc_common.configure_features(ctx = ctx, cc_toolchain = cc_toolchain)
    variables = cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        user_compile_flags = ctx.fragments.cpp.copts + ctx.fragments.cpp.cxxopts +
                             getattr(ctx.rule.attr, "copts", []),
        include_directories = compilation_context.includes,
        quote_include_directories = compilation_context.quote_includes,
        system_include_directories = depset(transitive = [
            compilation_context.system_includes,
            compilation_context.external_includes,
        ]),
        framework_include_directories = compilation_context.framework_includes,
        preprocessor_defines = depset(transitive = [local_defines, compilation_context.defines]),
    )
    flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
        variables = variables,
    )

    # clang-tidy is not the toolchain's compiler, so name its builtin include directories. Apple's
    # toolchain lists the SDK's C headers ahead of libc++'s, which libc++ rejects, so keep the C++
    # ones first.
    builtin = cc_toolchain.built_in_include_directories
    cxx = [d for d in builtin if "/c++/" in d or d.endswith("/c++")]
    return flags + [
        flag
        for d in cxx + [d for d in builtin if d not in cxx]
        for flag in ("-isystem", d)
    ] + ["-xc++"]

def _clang_tidy_impl(target, ctx):
    if CcInfo not in target or target.label.workspace_name:
        return []

    srcs = [
        f
        for attr in ("srcs", "hdrs")
        for t in getattr(ctx.rule.attr, attr, [])
        for f in t.files.to_list()
        if f.is_source and f.extension in ("cc", "h")
    ]
    if not srcs:
        return []

    compilation_context = cc_common.merge_compilation_contexts(compilation_contexts = [
        dep[CcInfo].compilation_context
        for dep in [target] + getattr(ctx.rule.attr, "implementation_deps", [])
    ])
    cc_toolchain = find_cpp_toolchain(ctx)
    flags = _compile_flags(
        ctx,
        cc_toolchain,
        compilation_context,
        target[CcInfo].compilation_context.local_defines,
    )

    # Every configuration is an input and none is named on the command line, so clang-tidy finds
    # each file's own .clang-tidy as it does outside Bazel.
    inputs = depset(
        ctx.files._configs,
        transitive = [compilation_context.headers, cc_toolchain.all_files],
    )
    outputs = []
    for src in srcs:
        out = ctx.actions.declare_file("{}.clang-tidy/{}".format(ctx.label.name, src.short_path))
        args = ctx.actions.args()
        args.add(out)
        args.add_all(["--quiet", src, "--"])
        args.add_all(flags)
        ctx.actions.run_shell(
            command = 'out=$1; shift; clang-tidy "$@" && touch "$out"',
            arguments = [args],
            inputs = depset([src], transitive = [inputs]),
            outputs = [out],
            mnemonic = "ClangTidy",
            progress_message = "Run clang-tidy on " + src.short_path,
            # clang-tidy comes from the developer's environment, such as `nix develop`.
            use_default_shell_env = True,
        )
        outputs.append(out)
    return [OutputGroupInfo(clang_tidy = depset(outputs))]

clang_tidy = aspect(
    implementation = _clang_tidy_impl,
    attrs = {
        # Lists every .clang-tidy in the repository.
        "_configs": attr.label(default = Label("//:clang_tidy_config")),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
