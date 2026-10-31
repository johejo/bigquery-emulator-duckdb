# s2geometry patches

`throw_delegate.patch` replaces `absl::base_internal::ThrowStdLengthError` and
`ThrowStdOutOfRange` with their public counterparts in `s2/util/gtl/compact_array.h`.

Abseil 20260526 deleted the internal `absl/base/internal/throw_delegate.h`, which s2geometry
0.14.0 includes, as GoogleSQL does; see [its patch](../googlesql/README.md). Drop the patch
once a release builds against Abseil 20260526.

`covering_order.patch` makes the region coverer's priority queue use libc++'s heap-pop order,
including its choice of the left child on an equal-priority tie. Upstream compares only priority,
leaving ties to the standard library's heap implementation. BigQuery's observed cell arrays match
libc++'s order, not libstdc++'s; a valid covering can otherwise differ from BigQuery. The patch
keeps this order independent of the build platform. The observed cases, including symmetric lines,
polygons, buffers and mixed geometries, are in
`tests/e2e/goclient/testdata/scalars/geography_covering*.txt`.

`buffer_libm.patch` makes `S2BufferOperation` compute its step angles with CORE-MATH's correctly
rounded `acos`, `tan` and `sin` rather than the platform's math library. A step one bit off moves
the vertices of a buffered linestring's round end cap, as macOS's library does, while BigQuery's
answers match correctly rounded functions; see [CORE-MATH](../core_math/README.md) and
`tests/e2e/goclient/testdata/scalars/geography_buffer.txt`.
