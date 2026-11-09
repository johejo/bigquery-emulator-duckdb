# UDF runtimes

See [architecture](architecture.md) for the query path, the [README](../README.md#python-udfs)
for configuration and security implications, and [Query and API limits](compatibility.md#user-defined-functions)
for supported forms.

## Composition and lifetime

[JavaScript](../src/udf/javascript/) and [Python](../src/udf/python/) runtimes have no DuckDB
dependency. Their [adapter](../src/udf_duckdb/) owns SQL conversion and query-lifetime state.
They keep separate interfaces because in-process QuickJS and Python worker processes have
different ownership and execution models.

The executable supplies registration through a backend initializer. Compliance omits it and
links neither runtime; this is independent of analyzer declaration policy. SQL UDFs remain in
the common analysis and translation path.

## Python workers

Each DuckDB local function state lazily owns a worker, reusing compiled functions across rows
but not sharing Python globals with other states. Query cleanup terminates the process group
and reaps the child. Only worker mode initializes CPython, before server or DuckDB initialization.

A private Unix socket carries batched requests separately from standard I/O, which points to
the null device. BYTES and nonfinite floats need explicit wire encodings. Frames are bounded
independently of batches; per-call progress renews the deadline so a large batch does not consume
one call's entire budget. Protocol details and limits live in [src/udf/python/](../src/udf/python/).

Memory enforcement differs by platform: Linux sets an address-space limit relative to startup
usage before Python initializes; macOS requires parent-side resident-memory sampling because
Darwin rejects `RLIMIT_AS`. Neither model is a security sandbox.
