# Architecture

SQL follows the path `Server` → GoogleSQL analysis → translation → DuckDB execution.
REST metadata operations and Storage RPCs share the same `Emulator` and catalog.

A query result keeps its rows as the DuckDB chunks of the statement's result, which a job keeps
for `jobs.getQueryResults`. A row is encoded as BigQuery's JSON cells only when a response reads
it, so a page encodes only its rows; the emulator's own reads, such as script variables and
extracts, read values from the vectors. Columns whose values can fail to
encode, such as an INTERVAL or BIGNUMERIC out of range, are encoded once when the statement
runs, so that the statement fails rather than a later read.

[`ResultChunks`](../src/result_chunks.h) owns the chunks and their database lifetime;
[`vector_value`](../src/vector_value.h) reads their physical values. Both targets are independent
of GoogleSQL. [`ResultRows`](../src/result_rows.h) encodes REST cells, while the separate
[`ReadGoogleSqlValue`](../src/result_googlesql.h) adapter converts values for scripts and
compliance tests. Encoders still use GoogleSQL where BigQuery's value validation or formatting
requires it.

A Storage Read session keeps its rows as the DuckDB vectors of one query, which selects the
requested fields in GoogleSQL, on a backend session the snapshot owns so that Arrow's connection
properties remain valid. Each response copies its rows of the columns whose DuckDB Arrow layout
matches the BigQuery wire type into a private chunk, exports them through the Arrow C Data
interface. A NULL array becomes an empty one, a BIGNUMERIC's stored bytes become a decimal256,
and infinite RANGE bounds become NULL. Columns with other physical layouts are appended to Arrow
arrays directly from their typed vectors. The resulting arrays are serialized as Arrow IPC with
Apache nanoarrow or encoded as Avro binary rows. Both formats use the table metadata to encode the public schema.

## Server

[`Server`](../src/server.h) owns the [REST](../src/server/rest.cc) and
[Storage gRPC](../src/server/storage.cc) transports; the `Emulator` must outlive it.
[REST routes](../src/server/routes.cc) come from the bundled BigQuery discovery document.
The transport validates parameter names and types, and handlers reject unsupported behavior.

## Frontend

[GoogleSQL](https://github.com/google/googlesql) resolves names and types into an AST, independently
of DuckDB execution. The main entry points are:

- [analyzer.cc](../src/analyzer.cc): statement analysis and query result schemas.
- [catalog.cc](../src/catalog.cc): table and graph name resolution through `TableSource`,
  and statement-local ownership of `EXTERNAL_QUERY`; [table_path.cc](../src/table_path.cc)
  applies the default project and dataset.
- [builtin_catalog.cc](../src/builtin_catalog.cc): construction and caching of shared builtin
  catalogs by declaration set and function exclusion list. [analyzer_functions](../src/analyzer_functions/) supplies
  BigQuery-specific function declarations missing from GoogleSQL, including argument constraints,
  defaults and TVF result schemas. It also builds `EXTERNAL_QUERY` with the statement's source,
  project and type factory. These declarations serve analysis, independently of the translation
  registry and backend registration.

`FunctionDeclarations` selects the analyzer declarations independently of builtin exclusions and
backend compatibility options. Normal execution adds BigQuery declarations; the compliance driver
selects only GoogleSQL builtins, without the additional declarations or `EXTERNAL_QUERY`. The
selection follows scripts, routine bodies and views through their statement-local catalogs.
- [type_mapping.cc](../src/type_mapping.cc): REST schema, GoogleSQL and DuckDB type conversions,
  which translation uses without linking DuckDB; [duckdb_types.cc](../src/duckdb_types.cc) reads
  the DuckDB types of results and stored columns.
- [information_schema.cc](../src/information_schema.cc): metadata rows computed during analysis
  and carried into the translated query. Dataset, table and routine views have separate
  sources, with column definitions beside their row builders.

For scripts, GoogleSQL's `ScriptExecutor` manages control flow and variables.
[script.cc](../src/script.cc) analyzes and executes each statement through `ScriptHost`, which
isolates it from emulator internals. Strict procedure definitions use
[procedure_validation.cc](../src/procedure_validation.cc) to analyze statements and expressions
with typed variables and their constant values, without executing SQL statements. Validation
stops at control statements, DDL, CALL and dynamic SQL, following BigQuery. Constant dynamic SQL
is evaluated by GoogleSQL and checked for syntax. Its routine catalog resolves references in
the procedure's own project, without job-local tables or functions.

## Translator

[translator.cc](../src/translator.cc) and [src/translator/](../src/translator/) turn the resolved
AST into DuckDB SQL. A statement-wide `Context` and per-scan `Scope` track columns and visibility.
Internal column IDs avoid alias collisions; translation preserves GoogleSQL semantics where
DuckDB differs, including NULL ordering and STRUCT comparisons.

The translator receives a read-only [function catalog](../src/function_catalog/catalog.h) from its
caller. The catalog owns registry entries and expands rules into SQL; it does not select feature
adapters. Each registration keeps its translation and `TypeUse` attributes together, with
argument restrictions in the overload rules. Calls use these attributes to reject unsupported
uses of physical representations such as graph STRUCTs or BIGNUMERIC values. Rule expansion
is the same for all backends; GEOGRAPHY support is a type attribute, independent of the backend
reported in the function support table.

Literal rule tables use [static definitions](../src/function_catalog/static_definitions.h): a
`constexpr` tuple owns each function's overload array beside its name and attributes. A separate
`constexpr` table takes views only after that tuple reaches static storage, so no view points into
a temporary or requires dynamic allocation. Registration copies these definitions into the catalog.

The common catalog also owns `ScalarCall`'s AST accessors and
[resolved_access](../src/function_catalog/resolved_access.h), which reads arguments, types and
literals for handlers. Feature providers can use them without depending on the translator;
the resolved AST definitions remain private to the implementation source. Its shared
[comparison helpers](../src/function_catalog/comparison.h) serve both STRUCT subqueries and
function handlers, keeping query translation independent of the builtin function definitions.

[emulator_functions.cc](../src/emulator_functions.cc) assembles the application's catalog from
[builtin definitions](../src/translator/functions.cc) and feature providers, one for each choice
of the backend functions' options, so that BigQuery's and GoogleSQL's semantics can translate
differently. The application registers COLLATE's handler here, keeping the translator independent
of the collation library. It checks the literal specification during translation, before DuckDB can
prune an expression or skip its evaluation; BigQuery's catalog takes only `und:ci` and the empty
specification, while GoogleSQL's takes the collator's specifications. The emulator and function
probe use this same composition.
[backend_functions.cc](../src/backend_functions.cc) registers the executable implementations with
DuckDB at the same application layer. The translator and common catalog do not depend on the
feature adapters' function-registration targets.

Geography's DuckDB adapter declares backend registrations and the SQL functions that call them
together. Both `RegisterGeographyFunctions` and the application's catalog composition consume
those definitions, including aliases and overloads that share an implementation. The adapter
and translator depend on the common `//src/function_catalog` rule types, construction and
expansion code. The `googlesql_geography` library remains independent of DuckDB.
[GoogleSQL math's adapter](../src/googlesql_functions_duckdb/math.cc) similarly shares a table
of public names, backend names, callbacks and arities for trigonometric functions between
DuckDB registration and SQL rule construction. Other math functions still use the builtin
backend rule table. See
[AGENTS.md](../AGENTS.md#implementing-functions) for backend selection and registration.

Graph definitions use [DuckPGQ](https://github.com/cwida/duckpgq-extension), linked
statically beside DuckDB. The graph translator turns resolved fixed paths into
joins on element tables and endpoint keys, and leaves property expressions and filters to the
ordinary SQL translator. Undirected edges match either orientation with OR over the same edge
row, preserving its properties and emitting self-loops once. Each path applies its own mode
before paths are joined by shared variables' identities. Private graph names encode the project,
dataset and graph name, since DuckPGQ's registry is shared across catalogs. Each project's
`main._property_graphs` table keeps canonical GoogleSQL DDL for catalog resolution
and native DDL for restoring DuckPGQ's registry on startup.
Query intermediates keep graph elements as structs containing a private identity from the
element table and its keys, labels, and a nested property struct. Edges also carry the identities
of their source and destination nodes, preserving the stored orientation through path construction
and later MATCH stages. Fixed paths contain separate
node and edge lists, with property types widened to the resolved path's element types. Graph
functions use this representation; it is never returned as an ordinary STRUCT to clients.

GoogleSQL's resolver propagates [collations](../third_party/bigquery_docs/reference/standard-sql/collation-concepts.md)
as type annotations and records the collation each operation uses in the resolved AST.
[collation.h](../src/translator/collation.h) is the translator's only reader of both. An operation
applies `und:ci` by comparing, ordering or grouping `bq_collation_key` BLOBs, the ICU sort keys of
GoogleSQL's collator, in place of the strings: functions marked `kComparesCollationKeys` key their
STRING arguments, and `ORDER BY` keys its items. Operations that do more than compare, such as
`STARTS_WITH` or `LIKE`, need GoogleSQL's collated implementations instead. Every other operation
rejects a non-empty collation, as do set operations, whose collation the resolved AST does not
record. A column's `FieldSchema::collation` becomes its annotation in the catalog, and a view's
columns take their query's annotations; a query result's or a new table's schema records the
collations of its columns' annotations.
[googlesql_collation](../src/googlesql_collation/) owns the collators and, behind the
`bigquery_collations` option, the specifications BigQuery takes; its DuckDB adapter registers the
key function and COLLATE's check of its specification.

## Backend

DuckDB executes translated SQL through its C++ API. Each project is a catalog and each dataset a
schema. Results are encoded in BigQuery's REST row format.
[googlesql_functions](../src/googlesql_functions/) owns scalar function compatibility logic,
including string validation, casts, date and time parsing and formatting, without depending on
DuckDB. [googlesql_functions_duckdb](../src/googlesql_functions_duckdb/) registers those functions
and upstream GoogleSQL functions, converts vectors and storage representations, and manages
aggregate callbacks. Upstream functions can be registered directly without an extra semantic wrapper.

Emulator implementations are also grouped by SQL feature in `src/googlesql_<feature>/`:
[geography](../src/googlesql_geography/), [AEAD and keysets](../src/googlesql_aead/),
[text analysis](../src/googlesql_text_analysis/) and [HLL sketches](../src/googlesql_hll/).
These libraries own input validation, computation and compatibility errors without depending on
DuckDB. Their `googlesql_<feature>_duckdb` adapters handle registration and DuckDB representations.
The `googlesql_` prefix denotes GoogleSQL-compatible semantics, including emulator implementations.
Shared DuckDB registration and vector helpers live in [duckdb_functions](../src/duckdb_functions/).
Where a backend function adapts GoogleSQL to a known BigQuery difference, such as CHR(0), FORMAT's
specifiers or GENERATE_ARRAY's limit, the library takes a flag for that difference and defaults
to GoogleSQL's behavior. The adapters read
[`functions_duckdb::Options`](../src/googlesql_functions_duckdb/options.h) at registration and
give each function only its own flags, as context the registration owns
(`RegisterWithContext` and `ApplyWithContext` in [duckdb_functions](../src/duckdb_functions/)).
[`BigQueryFunctionOptions()`](../src/function_options.h) selects every difference, and is the
default of `Backend`, `Emulator` and `RegisterBackendFunctions`.

ObjectRef validation lives in `googlesql_functions/objectref`; its DuckDB adapter receives
connection lookup and metadata fetch callbacks. [object_ref.cc](../src/object_ref.cc) owns the
GCS-backed metadata fetch, and [connection_id.cc](../src/connection_id.cc) normalizes connection IDs
for both ObjectRef and external queries. [backend_functions.cc](../src/backend_functions.cc)
assembles registration of these common function libraries. Application-specific registrations
are supplied through a [`BackendInitializer`](../src/backend_initializer.h).
[`service_backend`](../src/service_backend.cc) composes Spanner query registration with
that initializer and loads PostgreSQL/MySQL scanners through `DatabaseInitializer`.
A native `Backend` does not register external connectors by default.

[`DuckDbTableSource`](../src/duckdb_table_source.cc) owns native catalog resolution,
project aliases, location tracking, time travel, numeric-storage conversion and write
restrictions. The application supplies an
[`ExternalTableProvider`](../src/external_table_provider.h) for external operations;
[`service_table_provider`](../src/service_table_provider.cc) prepares GCS reads and
discovers/registers external connections, using the shared connection registry and
backend outside script transactions. Without a provider, external reads fail rather
than reading native placeholder tables. GCS read definitions retain metadata, schema
and the client by value; analysis does not stage files, and `StatementResources` keeps
staged files alive through result or Storage snapshot consumption. Metadata URI
validation uses [`gcs_uri`](../src/gcs_uri.cc), which has no service SDK dependency.

HLL uses a [port of zetasketch](../src/zetasketch/README.md) for BigQuery-compatible sketches;
the HLL library supplies BigQuery's input hashing, value type identifiers and precision and merge rules.
Text analysis keeps tokenization, document counts and scores in the library; its adapter encodes
the intermediate window dictionary and caches parsed analyzers and dictionaries.

GEOGRAPHY operations live in [googlesql_geography](../src/googlesql_geography/), a typed
function library over S2 with no DuckDB dependency. [googlesql_geography_duckdb](../src/googlesql_geography_duckdb/)
registers those functions and converts DuckDB arguments and results. Query values and stored
columns are BLOBs, under the `bq_geography` alias in columns, that hold the exact S2 vertices and
the encoded S2ShapeIndex of a geography's shapes; distance functions query that index without
rebuilding it, while predicates and set operations snap their inputs together first and build
their own. External WKT parsing and formatting belong to the function library. The emulator's backend composition selects compatibility options and passes
them through the adapter; registered callbacks own their options independently for each database.

BIGNUMERIC query values are BIGNUM integers in units of 10^-38; stored columns use the
`bq_bignumeric_units` alias or a precision-and-scale alias. `Backend::NumericReadQuery` converts
external integer and decimal columns to those units at the table boundary. Catalog scans, REST
row reads, copies and extracts share it; writes that retain external numeric storage are rejected.

`EXTERNAL_QUERY` exposes each connection through a DuckDB table macro:

- [MySQL](../src/mysql_query.cc): MariaDB Connector/C prepares the query during analysis; the
  linked [mysql_scanner extension](../third_party/duckdb_mysql/README.md) reads the rows.
- [PostgreSQL](../src/external_query.cc): libpq prepares the query during analysis; the linked
  [postgres_scanner extension](../third_party/duckdb_postgres/README.md) reads the rows.
- [Spanner](../src/spanner_query.cc): a `PLAN` RPC supplies result metadata; google-cloud-cpp
  streams rows into a DuckDB table function with one reader and bounded chunks.

### KLL sketches

[src/kll_duckdb/](../src/kll_duckdb/kll.cc) reads and writes BigQuery's KLL sketch BYTES:
a protobuf envelope, not Apache DataSketches' binary preamble, whose observed fields,
fixed-sketch EXTRACT answers and SQL validation cases [tools/kll/](../tools/kll/README.md)
keeps. The reader decodes INT64 exactly, includes the sampled value and weight, normalizes
ranks by retained mass rather than input count, and returns the stored extrema at the
endpoints; integer rank arithmetic avoids rounding INT64 values or array-quantile ranks
through FLOAT64. It bounds wire reads, varints, level shifts and accumulated weights,
rejects ambiguous duplicate singular fields, accepts absent encoding versions and explicit
versions 1/2, skips unknown varint/length-delimited fields, and rejects other wire forms
and versions.

INIT and MERGE share one writer: unbiased randomized pair compactors with a fixed capacity
of `64 * precision` items per level and exact, separately stored extrema. Weighted inserts
split a weight into powers of two, and merging imports each retained weight, including a
native sketch's sampler mass, without expanding it into rows. Input count and retained
mass stay separate. A merge keeps the first non-empty sketch's precision and lowers it
to the smaller one only when the incoming sketch has been compacted, following the
documented exception for inputs small enough to be captured exactly; BigQuery kept the
first sketch's precision for exact inputs in either order
([merge precision answers](../tools/kll/merge_precision.answers.txt)). The writer does not
reproduce BigQuery's capacity formula, compaction thresholds or random sequence, so its
bytes differ from BigQuery's for the same input; BigQuery reads and merges them
([writer answers](../tools/kll/writer.answers.txt)). The fixed capacity trades space for a
simple error argument; capacities that shrink toward the top level, as in KLL, are deferred
until each precision's rank error is justified for them.

A typed MERGE or MERGE_POINT merges as MERGE_PARTIAL does and extracts from the result
with EXTRACT's reader. A translator handler and the aggregate rules enforce the constant
number-of-quantiles and phi restrictions, which GoogleSQL's builtin signatures do not
impose; the backend validates the argument range before propagating a NULL sketch.

## Metadata and routines

[duckdb_catalog.cc](../src/duckdb_catalog.cc) reads metadata and removes expired relations using
the supplied backend, including uncommitted script changes.
[duckdb_table_source.cc](../src/duckdb_table_source.cc) adapts it to `TableSource`, tracks job
locations and registers external connection macros. Neither depends on `Emulator` or the parser.

Resource operations live in `emulator_datasets.cc`, `emulator_tables.cc` and `emulator_routines.cc`;
`emulator_tabledata.cc` handles rows, and `emulator_ddl.cc` handles query DDL metadata.
Mutations and their response reads share the expiration mutex; expiration drops recheck metadata
within their transaction.

Table and column comments store BigQuery metadata. Dataset metadata lives in
`main.emulator_datasets` because DuckDB cannot comment on schemas. Catalog changes and their
metadata writes share a transaction.

[table_history.cc](../src/table_history.cc) keeps a table's rows before each write for `FOR
SYSTEM_TIME AS OF`, in tables of the project's `main` schema keyed by dataset, table and creation
time. Writes outside a transaction hold a mutex so that each kept version is what the previous
write committed; a transaction dates its versions when it commits.

Logical views are native DuckDB views whose comments preserve the original GoogleSQL definition
and result schema. Materialized views use the same execution path, with their own object type
and base-table creation identity recorded in the comment. Reads compute current results, and
recreating the base table invalidates its materialized views. Persistent SQL UDFs, table functions
and procedures use macro comments.
For analysis, `RoutineCatalog` resolves view and routine definitions against their owning project
and dataset, shares their dependency state and detects recursion. This lets queries use current
definitions while keeping job-local objects out of persistent dependencies.

## Function type probes

The existing `//:function_probe` enumerates GoogleSQL signatures and builds calls using
`tools/function_probe_hints.txt`; `tools/restprobe functions` aggregates their outcomes per
function. That is separate from the specification-driven type coverage in
[function-types.md](function-types.md).

`tools/functioncoverage` is a Go generator for reviewed classes of type tables and prose domains
in the saved BigQuery documentation. `just function-types-update` generates
`tools/function_types.json`, a reviewable list of concrete argument and result types with local
source pages. It extracts unary INPUT/OUTPUT tables and binary input matrices from mathematical
and aggregate function pages, with explicit rules for synonyms and optional arguments. Unknown
table shapes or types fail generation. `domains.go` expands reviewed STRING/BYTES, numeric,
orderable, array-element and arbitrary-value domains, checking that their source sections exist.
These prose rules require review when the specification changes; they are not inferred from
emulator registrations. Generic domains use representative nested structural shapes; their
limits and other incomplete expansions are stated in the report and catalog notes.
To expand coverage, add a reviewed extraction or domain rule and its source snapshot, regenerate
the catalog, and review the concrete signatures. Keep function-specific valid samples in the
catalog's generation rules and recursive typed samples in `internal/functioncoverage`.

`tools/restprobe function-types` binds data arguments as typed columns, executes every catalog
form and compares the REST result schema with the documented type, recursively including
STRUCT fields, repeated fields and RANGE element metadata. Missing samples or analyzer
rejections remain visible as Untested; functions absent from the catalog are Not enumerated.
Syntax-like options can explicitly remain literals. A function cannot be Supported when an
enumerated form is Untested. The JSON catalog stays a single file; Markdown has an overview in
`docs/function-types.md` and detail pages under `docs/function-types/`, grouped by specification
page. The overview links directly to each function's details, and detail pages link back to the
shared coverage notes. `just docs-function-types` regenerates this report set alone;
`just docs` includes it, and `just docs-check` checks the catalog and every report file for
freshness offline. `restprobe -check function-types CATALOG FUNCTIONS_FILE OUTPUT_DIR` checks
without modifying files, including detecting obsolete domain pages; regeneration removes them.
These probes do not verify result values or establish semantic compatibility.

## Collation probes

`tools/collationcoverage` lists the sections of the saved function, expression and operator
references that say they support collation; `just collation-operations-update` writes them to
`tools/collation_operations.json`. `//:function_probe` calls each listed function with a collated
argument in every signature GoogleSQL marks as using the operation collation, and fails for a
listed function no call collates, so that the documentation and GoogleSQL's signatures cannot
drift apart unnoticed. A function whose collated call the emulator rejects is Partial in
[functions.md](functions.md). The other listed operations, and where a collation can be
assigned, are the Collation section of `tools/sql_features.txt`.

## DDL option probes

`tools/ddl_options.json` is a snapshot of the option lists in BigQuery's DDL reference. `groups`
maps each list's section anchor to its options: a name, a GoogleSQL type and SQL sample `values`.
Each of `contexts` names a statement that takes a group's options, with a `template` holding
`{option}` once, `setup` statements, and `only` or `exclude` to keep the options a language or
form takes. `implementation` names the DDL handlers' context in `src/ddl_option_types.cc`.

`internal/ddloptions` checks the catalog's structure and builds one statement per sample, which
`tools/restprobe options` runs on fresh emulators to generate `docs/options.md`.
`tests/ddl_options_catalog_test.cc` parses each type and sample with the emulator's analyzer
settings, and compares types with the handlers' definitions, which must all appear in the catalog.

`just update-ddl-options` refreshes names and types from the reference's snapshot under
`third_party/bigquery_docs/` with `tools/ddloptions`, keeping samples and contexts. A new option
has no samples, and a new list a context without a template, so both stay Untested until someone
adds samples that meet the reference's constraints.

See [compatibility.md](compatibility.md) for query and API limits and
[storage-api.md](storage-api.md) for Storage client behavior.

### UDF runtimes

[udf/javascript](../src/udf/javascript/) owns the QuickJS engine, compiled-source cache and
execution limits. [udf/python](../src/udf/python/) owns Python worker discovery, process management,
communication and limits. Neither runtime depends on DuckDB. [udf_duckdb](../src/udf_duckdb/)
converts SQL values, registers the UDF calls and ties one runtime per thread to the query lifetime.
The runtimes keep separate interfaces because their execution and ownership models differ.
[udf_duckdb/register.cc](../src/udf_duckdb/register.cc) captures their configuration in a backend
initializer, which the executable supplies to `Emulator` in both server and shell modes. The
common backend does not depend on either runtime; compliance omits the initializer and links
neither QuickJS nor the Python worker runtime. This execution composition is independent of the
catalog's declaration policy and leaves SQL UDF analysis and translation in the common path.

Python routine definitions are stored in the same macro comments as SQL routines, with their
runtime and entry point. The catalog exposes them as volatile functions, and the translator
calls typed `bq_py_*` scalar functions. Each DuckDB local function state starts a worker lazily,
reuses its compiled functions across rows, and destroys it at query cleanup. Independent
execution states use separate processes and Python globals.

The emulator statically links CPython 3.11 from the full python-build-standalone distribution
(see [third_party/python](../third_party/python/)) and re-executes itself with
the `internal-python-worker` subcommand for each worker. Only this internal mode initializes libpython,
before any server or DuckDB initialization. An explicit `--python-worker` still starts an
external executable with the memory limit as its only argument. The worker uses the CPython C API for compilation,
argument conversion and calls. A private Unix socket carries newline-delimited JSON batches,
with hex for BYTES and strings for floating-point values so nonfinite values survive transport.
Definitions and argument schemas are sent once per worker, then referenced by worker-local IDs.
Requests group up to 256 rows within a DuckDB chunk, targeting at most 1 MiB of encoded rows;
a single larger row is allowed within the 64 MiB frame limit. Results use separate frames,
targeting 1 MiB, so the frame limit does not cap a batch's combined results. An empty line
acknowledges each completed scalar call; the parent renews the call deadline on progress without
requiring a round trip before the next row. An error stops the batch and fails the query.
Standard input, output and error point to the null device, separately from the protocol socket.
The parent polls nonblocking I/O against a call deadline and kills the worker process group on cleanup, then
waits for its child. On Linux, the worker reads its initial address space from `/proc/self/statm`
and sets `RLIMIT_AS` to that baseline plus the configured budget before Python initializes.
This excludes the emulator executable and its startup mappings from the budget. On macOS,
where Darwin rejects `RLIMIT_AS`, the parent samples the worker's resident memory while polling.
