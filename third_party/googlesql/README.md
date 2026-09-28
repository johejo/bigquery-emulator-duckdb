# googlesql patch

`throw_delegate.patch` replaces `absl::base_internal::ThrowStdOutOfRange` with the public
`absl::ThrowStdOutOfRange` in `googlesql/base/associative_view_internal.h`.

Abseil 20260526 deleted the internal `absl/base/internal/throw_delegate.h`, and
google_cloud_cpp 3.9+ requires that Abseil version, so GoogleSQL 2026.9.2 does not build
against it without this.

GoogleSQL does not accept external contributions. Drop the patch once a release exported
from upstream builds against Abseil 20260526.
