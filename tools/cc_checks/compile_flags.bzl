"""Compiler flags shared by C++ checking aspects."""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

def compile_flags(ctx, cc_toolchain, compilation_context, local_defines, builtin_includes = False):
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        # Standalone headers do not have the compiler's module-map action. Dependency layering
        # is checked by the real build, not by these aspects.
        unsupported_features = ctx.disabled_features + ["layering_check", "module_maps", "header_modules"],
    )

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
    env = cc_common.get_environment_variables(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
        variables = variables,
    )

    if not builtin_includes:
        return flags + ["-xc++"], env

    # clang-tidy is not the toolchain's compiler, so name its builtin include directories. Apple's
    # toolchain lists the SDK's C headers ahead of libc++'s, which libc++ rejects, so keep the C++
    # ones first.
    builtin = cc_toolchain.built_in_include_directories
    cxx = [d for d in builtin if "/c++/" in d or d.endswith("/c++")]
    return flags + [
        flag
        for d in cxx + [d for d in builtin if d not in cxx]
        for flag in ("-isystem", d)
    ] + ["-xc++"], env
