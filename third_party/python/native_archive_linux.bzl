"""Convert CPython's full-LTO archive with an ELF relocatable link."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cpp_toolchain", "use_cc_toolchain")

def _linux_native_archive_impl(ctx):
    toolchain = find_cpp_toolchain(ctx)
    archive = ctx.actions.declare_file(ctx.label.name + ".a")
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

linux_native_archive = rule(
    implementation = _linux_native_archive_impl,
    attrs = {
        "src": attr.label(allow_single_file = [".a"], mandatory = True),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
