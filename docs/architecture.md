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
[builtin definitions](../src/translator/functions.cc) and feature providers. The emulator and
function probe use this same composition. [backend_functions.cc](../src/backend_functions.cc)
registers the executable implementations with DuckDB at the same application layer. The translator
and common catalog do not depend on the feature adapters' function-registration targets.

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

### KLL sketch format investigation

KLL is not implemented. Unlike HLL, it has no implementation to port from the public
ZetaSketch repository. Maintainers have recorded the initial BigQuery answers in
[tools/kll/](../tools/kll/README.md). The observations below distinguish decoded wire
structure from inferred semantics; they do not yet constitute a complete codec.

Public sources inspected:

- [ZetaSketch README](https://github.com/google/zetasketch/blob/d098c49efb5a5bc70eec89c028a38b14b930ca98/README.md)
  lists only HLL++. Its
  [aggregator.proto](https://github.com/google/zetasketch/blob/d098c49efb5a5bc70eec89c028a38b14b930ca98/proto/aggregator.proto)
  defines SUM and HLL++ algorithm identifiers, but no KLL payload or algorithm identifier.
  The existing [C++ port](../src/zetasketch/README.md) is therefore not a KLL backend.
- GoogleSQL declares the functions in
  [builtin_function_sketches.cc](https://github.com/google/googlesql/blob/81c15b796138185c046aefe852321011fdf96615/googlesql/common/builtin_function_sketches.cc).
  Its public tree has KLL compliance SQL, but no KLL-named implementation or serialization
  proto; [reference_impl/function.cc](https://github.com/google/googlesql/blob/81c15b796138185c046aefe852321011fdf96615/googlesql/reference_impl/function.cc)
  supplies no KLL implementation. Declarations and compliance cases do not supply a codec.
- Apache DataSketches has an existing C++ KLL implementation, but its
  [serializer](https://github.com/apache/datasketches-cpp/blob/be33db564a4011bded6ded7b09d6d27b08ef4984/kll/include/kll_sketch_impl.hpp)
  writes a DataSketches preamble: version, family 15, flags, `k`, `m`, and, for
  non-singleton states, counts, levels, extrema and retained items. This is not evidence
  of BigQuery compatibility. The
  [sketch class](https://github.com/apache/datasketches-cpp/blob/be33db564a4011bded6ded7b09d6d27b08ef4984/kll/include/kll_sketch.hpp)
  uses a `uint16_t k` and rejects NaN items before insertion. BigQuery's precision is not
  established to equal `k`, and NaN retention needs more than a comparator change.
- [DataSketches for BigQuery](https://github.com/apache/datasketches-bigquery/blob/9f86fbede303815f07706bb8f88d0c49c41c5675/kll/README.md)
  exposes separate `kll_sketch_float_*` UDFs. Their availability inside BigQuery does not
  establish interoperability with native `KLL_QUANTILES.*` sketches.
- [googlesqlite's KLL implementation](https://github.com/Flip-to/googlesqlite/blob/ecf58b379d48f0364889fc28c838854253218a31/internal/functions/kll/sketch.go)
  explicitly stores all values as float64 in a private Go gob encoding and disclaims
  BigQuery sketch interoperability. It is not a compatible codec or an INT64 backend.

The saved [BigQuery function reference](../third_party/bigquery_docs/reference/standard-sql/kll_functions.md)
identifies KLL16, BYTES input/output, non-determinism, precision and weight limits, and
mixed-precision merge behavior. It does not specify wire fields. The following field
numbers come from decoding the recorded BigQuery bytes, not from that documentation.

#### Observed wire structure

The [wire specimens](../tools/kll/sketch_format.answers.txt) use a protobuf envelope
consistent with ZetaSketch's `AggregatorStateProto`, not the DataSketches preamble:

| Outer field | Wire type | Observed value / interpretation |
| --- | --- | --- |
| 1 | varint | `113`, KLL algorithm identifier |
| 2 | varint | Input count, including weights |
| 3 | varint / absent | Absent in original specimens; explicit versions `1` and `2` were both accepted in the singleton follow-up |
| 4 | varint | `4` for INT64, `10` for FLOAT64 |
| 113 | length-delimited | KLL payload, tag bytes `8a07` |

Within payload field 113:

| Field | Wire type | Observed value / interpretation |
| --- | --- | --- |
| 1 | varint | Precision-dependent parameter, plausibly a capacity; formula not established |
| 2 | varint | Requested precision |
| 3 | length-delimited | Minimum value bytes |
| 4 | length-delimited | Maximum value bytes |
| 5 | repeated length-delimited | Level messages, including empty levels |
| 6 | length-delimited | Additional nested state; its value and weight fit the sampler model below |

Payload field 1 maps precision `1, 2, 100, 1000, 2000, 100000` to
`3, 5, 280, 2954, 6008, 330058` in the singleton specimens. It is not simply the
precision, nor established to be DataSketches' `k`. The exact-state mixed-precision
merge retained `1000` and `2954`; compacted `1`/`1000` merges recorded `1` and `3`.
These observations do not settle the general downgrade rule or merge-order dependence.

For INT64, extrema use two's-complement varints (not ZigZag). Level field 2 contains
concatenated varints: the first value followed by nonnegative deltas in sorted order.
Accumulating modulo 2^64 and interpreting the result as signed reconstructs all recorded
integer levels, including `[INT64_MIN, 0, INT64_MAX]`. FLOAT64 extrema and level field 1
contain little-endian IEEE754 doubles; the five-item specimen decodes in sorted order.
NaN survives serialization. The negative-zero and positive-zero input queries produced
identical positive-zero bytes; this does not locate where zero normalization occurred.

Payload field 6 contains length-delimited field 1 and varint fields 2 and 3 in several
compacted specimens; the maximum-weight specimens contain only field 3 (`13`). The
[fixed-byte follow-up answers](../tools/kll/followup.answers.txt) now support this
**candidate extraction model**:

1. Decode and combine level items, assigning weight `2^level` to each item.
2. Add field `6.1` as another value with weight `6.2`, when that weight is nonzero.
   Its encoding matches the type's extrema encoding, not the level delta encoding.
3. Sort the combined values and normalize by their **total retained weight**, not
   outer field 2's input count. Return the first value whose cumulative weight is at
   least `phi * total_weight`; for array quantile `i/k`, this is rank
   `ceil(i * total_weight / k)`.
4. At phi 0 and 1, return the stored exact minimum and maximum, respectively.

This model matches all 13 fixed-byte EXTRACT responses in that round, including exact,
weighted, compacted and mixed-precision merged sketches. Examples of the decoded mass:

| Original input / precision | Level mass | Field 6 value | Field 6 weight | Total retained weight |
| --- | --- | --- | --- | --- |
| INT64 1..4 / 1 | 4 | 4 | 1 | 5 |
| INT64 1..16 / 1 | 8 | 16 | 7 | 15 |
| INT64 1..1024 / 1 | 1024 | 1014 | 29 | 1053 |
| FLOAT64 -256..256, step 1/8 / 1 | 4096 | 220.625 | 854 | 4950 |
| Merged INT64, input count 8192 / 1 | 4096 | 2605 | 1539 | 5635 |
| Merged FLOAT64, input count 8192 / 1 | 8192 | 393.125 | 1025 | 9217 |

The [sampler boundary answers](../tools/kll/sampler.answers.txt) distinguish this model
from normalization by original input count:

- For the four-input specimen with retained mass 5, phi `[.4,.400001,.8,.800001]`
  returned `[1,3,3,4]`, crossing at retained cumulative masses 2/5 and 4/5.
- For the sixteen-input specimen with retained mass 15, phi `[.5,.51,.533,.534]`
  returned `[4,4,4,16]`, consistent with the boundary 8/15 rather than 8/16.
- The compacted FLOAT64 specimen returned `[-186.25,75.875,75.875,220.625]` at
  phi `[.413,.414,.827,.828]`, consistent with boundaries 2048/4950 and 4096/4950.
- Changing only sampler weight `7` to `0` in the sixteen-input specimen was accepted;
  phi .75 returned `4`, consistent with excluding its sampled value `16`.
- Changing only field `6.3` from `3` to `4` was accepted and left that specimen's
  nine-quantile extraction unchanged. This does not establish that arbitrary values
  are valid, or that the field is irrelevant to future updates or merges.

All five sampler responses match the candidate extraction model. They strengthen the
reader interpretation, but do not prove it for all encodings or floating-point rank
boundaries. Field `6.3` remains exponent-like state with unresolved update/compaction
semantics. Do not impose a retained-weight equality with input count: even the
precision-1000, 16384-item specimen has level mass `16383` without field 6. These are
not grounds to reject it.

NULL-only and empty INITs returned NULL, not serialized empty sketches. A one-input
MERGE_PARTIAL preserved the particular exact specimen byte-for-byte; this is not a
promise of deterministic serialization. Empty/malformed bytes, a truncated singleton,
malformed trailing `ff`, and an HLL sketch all produced `Invalid input sketch bytes`.
Appending well-formed unknown varint field `1000=1` to the recorded singleton was
accepted. Explicit envelope versions 1 and 2 also extracted that singleton unchanged;
this does not establish acceptance of arbitrary versions or other unknown fields.

#### Observed SQL behavior and implementation boundary

The [behavior answers](../tools/kll/quantiles.answers.txt) also establish:

- EXTRACT_POINT's quantile argument must be constant. The original per-row-phi queries
  failed analysis and establish no rank-selection rule. Precision must be a literal or
  query parameter and cannot be NULL; EXTRACT quantile arguments cannot be NULL either.
- A NULL weight skips that input. A NULL input with weight zero was skipped, but invalid
  precision zero failed even on an empty population.
- Empty MERGE_INT64/FLOAT64 returned `[]`; empty MERGE_POINT variants returned NULL.
- INT64/FLOAT64 sketch type mismatches fail rather than convert.
- SAFE is rejected for INIT aggregates and EXTRACT_INT64. SAFE.EXTRACT_POINT with a
  negative literal phi still failed argument validation; this does not demonstrate
  runtime SAFE error handling for that function.
- The NaN-containing population preserved NaN as the first quantile; NaN-only extraction
  returned NaN. The recorded large weighted constant population extracted only `42`.

The completed [follow-up answers](../tools/kll/followup.answers.txt) correct the
constant-phi queries. On the exact population `[10,20,30,40]`, both INT64 and FLOAT64
returned `[10,10,10,20,20,30,40]` for phi `[0,.249,.25,.251,.5,.75,1]`. This agrees
with nearest-rank selection `max(1, ceil(phi*n))`, not interpolation or rounding an
index based on `n-1`. Exact full-range INT64 extraction also preserves all 64 bits.
The fixed compacted-state outputs are now independent reader expectations, and merging
two fixed compacted sketches preserved their exact overall extrema `[1,16]`.

All three collection rounds are complete; the 86 SQL observations, including errors,
are archived in [tools/kll/](../tools/kll/README.md), with no pending KLL queries in
`unverified/`. The current corpus is enough to begin a separately scoped reader:
protobuf decoding, typed extrema and levels, sampled value/weight, and EXTRACT/EXTRACT_POINT.
Use the recorded fixed-byte answers as independent client-visible tests. Keep INT64
values exact, use GoogleSQL's FLOAT64 ordering including NaN, and treat version/unknown-field
acceptance only as far as it has been observed. A reader must still check malformed
inputs safely and reject unimplemented encoding forms explicitly.

Never use a locally decoded percentile as its own expected result, or promote observed
INIT/MERGE_PARTIAL hex to a golden writer result. These reader observations do not
supply the precision-to-capacity formula, compaction/update algorithm or general merge
rules. INIT and all MERGE variants therefore remain a separate implementation problem,
not a wrapper around the reader. Once a writer exists, prepare an interoperability
round for BigQuery to EXTRACT and MERGE its bytes, including both types and compacted
states. No writer or KLL function registration is added by this investigation.

A DataSketches backend would still need evidence for the precision/error mapping,
weighted updates, NaN retention/order, merge rules and both directions of BYTES
interoperability. Until then, do not register a private-format substitute as native KLL.

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
