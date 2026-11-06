# nanoarrow

Apache nanoarrow 0.7.0 provides Arrow arrays and IPC serialization for the Storage API.
`BUILD.nanoarrow.bazel` builds the upstream C sources and vendored flatcc runtime directly;
device extensions and built-in decompression are not enabled. Sources and licenses come from
the pinned upstream archive in `MODULE.bazel`.

`compression.patch` adds a record-batch encoder entry point with a buffer compression callback
and writes the IPC body compression metadata. The Storage Read codec supplies LZ4_FRAME and ZSTD
compression; nanoarrow retains buffer layout, padding and nested-array traversal.

`public_headers.patch` marks the public umbrella headers’ includes as exports for clang-tidy;
it changes comments only.
