# nanoarrow

Apache nanoarrow 0.7.0 provides Arrow arrays and IPC serialization for the Storage API.
`BUILD.nanoarrow.bazel` builds the upstream C sources and vendored flatcc runtime directly;
compression and device extensions are not enabled. Sources and licenses come from the pinned
upstream archive in `MODULE.bazel`.

`public_headers.patch` marks the public umbrella headers’ includes as exports for clang-tidy;
it changes comments only.
