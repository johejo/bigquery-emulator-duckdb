# s2geometry patch

`throw_delegate.patch` replaces `absl::base_internal::ThrowStdLengthError` and
`ThrowStdOutOfRange` with their public counterparts in `s2/util/gtl/compact_array.h`.

Abseil 20260526 deleted the internal `absl/base/internal/throw_delegate.h`, which s2geometry
0.14.0 includes, as GoogleSQL does; see [its patch](../googlesql/README.md). Drop the patch
once a release builds against Abseil 20260526.
