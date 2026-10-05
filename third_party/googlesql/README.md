# googlesql patch

`throw_delegate.patch` replaces `absl::base_internal::ThrowStdOutOfRange` with the public
`absl::ThrowStdOutOfRange` in `googlesql/base/associative_view_internal.h`.

Abseil 20260526 deleted the internal `absl/base/internal/throw_delegate.h`, and
google_cloud_cpp 3.9+ requires that Abseil version, so GoogleSQL 2026.9.2 does not build
against it without this.

GoogleSQL does not accept external contributions. Drop the patch once a release exported
from upstream builds against Abseil 20260526.

`compliance.patch` lets `//:compliance_test` run GoogleSQL's compliance tests. It adds the main
repository to the `googlesql_implementation` package group, which guards the test-only targets
under `googlesql/compliance`, and marks `compliance_test_cases` `alwayslink`: nothing references
its test cases, which register themselves, so the linker would otherwise drop them all. GoogleSQL
also looks for its `.test` files and test protos in the runfiles of the main repository, `_main`,
and of `protobuf~` and `googleapis~`; the patch points those at the canonical names of the modules,
`googlesql+`, `protobuf+` and `googleapis+`. Last, it has GoogleSQL's `http_archive` of
file-based-test-driver fix `Match` and `GetContents`, which pass the data of a `string_view` that is
not NUL-terminated to `stat()` and so fail to find a `.test` file at random. It stays as long as
that test does.
