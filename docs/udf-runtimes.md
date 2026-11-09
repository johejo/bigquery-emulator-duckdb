# UDF runtimes

This page describes worker ownership and execution. See the [architecture](architecture.md)
for the shared query path, the [README](../README.md#python-udfs) for configuration, and
[Query and API limits](compatibility.md#user-defined-functions) for supported forms.

## Composition and lifetime

[udf/javascript](../src/udf/javascript/) owns QuickJS, its compiled-source cache and execution
limits. [udf/python](../src/udf/python/) owns worker discovery, process management,
communication and limits. Neither depends on DuckDB.
[udf_duckdb](../src/udf_duckdb/) converts SQL values, registers calls and ties one runtime per
thread to the query lifetime. The runtimes have separate interfaces because their execution
and ownership models differ.

[udf_duckdb/register.cc](../src/udf_duckdb/register.cc) captures configuration in a backend
initializer supplied by the executable in server and shell modes. The common backend depends
on neither runtime; compliance omits the initializer and links neither QuickJS nor the Python
worker runtime. This composition is independent of catalog declaration policy; SQL UDF analysis
and translation stay in the common path.

## Python workers

Python routine definitions, runtime and entry point are stored in macro comments, like SQL
routines. The catalog exposes volatile functions; translation calls typed `bq_py_*` scalar
functions. Each DuckDB local function state lazily starts a worker, reuses compiled functions
across rows and destroys it at query cleanup. Independent states have separate processes and
globals.

The executable statically links CPython 3.11 from
[python-build-standalone](../third_party/python/) and re-executes itself with
`internal-python-worker`. Only that mode initializes libpython, before server or DuckDB
initialization. `--python-worker` instead starts an external executable with the memory limit
as its only argument. The worker uses the CPython C API for compilation, conversion and calls.

A private Unix socket carries newline-delimited JSON batches, hex BYTES and string-encoded
floating-point values to preserve nonfinite values. Definitions and schemas are sent once,
then referenced by worker-local IDs. Requests group up to 256 rows within a DuckDB chunk,
targeting 1 MiB encoded; a single larger row is allowed within the 64 MiB frame limit. Results
use separate frames targeting 1 MiB, so combined batch results are not capped by one frame.
An empty line acknowledges each scalar call, renewing the deadline on progress without a
round trip before the next row. Errors stop the batch and fail the query.

Standard input, output and error point to the null device, separately from the protocol socket.
The parent polls nonblocking I/O against the call deadline, kills the worker process group on
cleanup and waits for its child. On Linux the worker reads startup address space from
`/proc/self/statm` and sets `RLIMIT_AS` to that baseline plus the budget before Python initializes.
On macOS, where Darwin rejects `RLIMIT_AS`, the parent samples resident memory while polling.
The observable limits and security implications are documented in the
[README](../README.md#python-udfs) and [compatibility guide](compatibility.md#python-udfs).
