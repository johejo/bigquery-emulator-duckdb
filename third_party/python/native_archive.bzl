"""Compile CPython's full-LTO archive once instead of at every executable link."""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cpp_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

def _macos_objects(ctx, toolchain):
    objects = ctx.actions.declare_directory(ctx.label.name + ".lto")
    dylib = ctx.actions.declare_file(ctx.label.name + ".dylib")
    features = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    variables = cc_common.create_link_variables(
        cc_toolchain = toolchain,
        feature_configuration = features,
        output_file = dylib.path,
        is_linking_dynamic_library = True,
        user_link_flags = ctx.fragments.cpp.linkopts + [
            "-fuse-ld=lld",
            "-nostdlib",
            "-Wl,-all_load",
            "-Wl,-undefined,dynamic_lookup",
            "-Wl,-object_path_lto," + objects.path,
            ctx.file.src.path,
        ],
    )
    action = ACTION_NAMES.cpp_link_dynamic_library
    args = cc_common.get_memory_inefficient_command_line(
        feature_configuration = features,
        action_name = action,
        variables = variables,
    )
    env = cc_common.get_environment_variables(
        feature_configuration = features,
        action_name = action,
        variables = variables,
    )

    # Mach-O lld has no relocatable link. Keep its LTO objects instead; the dylib is
    # discarded. All members of the pinned macOS libpython archive are bitcode.
    # Exporting the dylib's symbols keeps Python's extension-module entry points,
    # and dynamic_lookup leaves external references for the final executable link.
    ctx.actions.run(
        executable = cc_common.get_tool_for_action(feature_configuration = features, action_name = action),
        arguments = args,
        inputs = depset([ctx.file.src], transitive = [toolchain.all_files]),
        outputs = [objects, dylib],
        env = env,
        mnemonic = "PythonNativeObject",
        progress_message = "Compile CPython's LTO archive to native code %{label}",
        use_default_shell_env = True,
    )
    return objects

def _native_archive_impl(ctx):
    toolchain = find_cpp_toolchain(ctx)
    archive = ctx.actions.declare_file(ctx.label.name + ".a")
    if ctx.target_platform_has_constraint(ctx.attr._macos[platform_common.ConstraintValueInfo]):
        objects = _macos_objects(ctx, toolchain)
        ctx.actions.run_shell(
            command = '"$1" rcs "$2" "$3"/*.o',
            arguments = [toolchain.ar_executable, archive.path, objects.path],
            inputs = depset([objects], transitive = [toolchain.all_files]),
            outputs = [archive],
            mnemonic = "PythonNativeArchive",
            progress_message = "Archive CPython's native code %{label}",
            use_default_shell_env = True,
        )
        return [DefaultInfo(files = depset([archive]))]

    obj = ctx.actions.declare_file(ctx.label.name + ".o")
    args = ctx.actions.args()
    args.add_all([
        "-fuse-ld=lld",
        "-flto",
        "-nostdlib",
        "-r",
        "-Wl,--whole-archive",
    ])
    args.add(ctx.file.src)
    args.add("-Wl,--no-whole-archive")
    args.add("-o", obj)
    ctx.actions.run(
        executable = toolchain.compiler_executable,
        arguments = [args],
        inputs = depset([ctx.file.src], transitive = [toolchain.all_files]),
        outputs = [obj],
        mnemonic = "PythonNativeObject",
        progress_message = "Compile CPython's LTO archive to native code %{label}",
        use_default_shell_env = True,
    )
    ctx.actions.run(
        executable = toolchain.ar_executable,
        arguments = ["rcs", archive.path, obj.path],
        inputs = depset([obj], transitive = [toolchain.all_files]),
        outputs = [archive],
        mnemonic = "PythonNativeArchive",
        progress_message = "Archive CPython's native code %{label}",
        use_default_shell_env = True,
    )
    return [DefaultInfo(files = depset([archive]))]

native_archive = rule(
    implementation = _native_archive_impl,
    attrs = {
        "src": attr.label(allow_single_file = [".a"], mandatory = True),
        "_macos": attr.label(default = "@platforms//os:macos"),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
