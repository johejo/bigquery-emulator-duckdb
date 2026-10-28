# OpenSSL

`openssl.patch` switches cpp-httplib's Bazel dependency from BoringSSL to OpenSSL, matching the
emulator's other TLS users and embedded CPython. Both `:ssl` and `:crypto` are direct dependencies
because OpenSSL's Bazel module exports the public SSL headers from `:crypto`.

# Thread pool

`thread_pool.patch` backports cpp-httplib v0.62.0's fix for its thread pool, which spawned a thread
only when none was idle. Threads notified for an earlier job still counted as idle, so a burst of
connections could leave one queued with no thread, waiting until a kept-alive connection closed.
Drop it on updating cpp-httplib to v0.62.0 or later.
