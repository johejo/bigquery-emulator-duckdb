# AGENTS.md

C++20 built with Bazel; prefer the standard library. `nix develop` provides the toolchain,
including `bq`. Use `just` recipes (see the `Justfile`) for building, testing, formatting and
generating docs.

Bazel rejects cyclic dependencies; do not break a cycle with a target that holds only headers.
Move what causes it, such as a type or an error class, to a lower target, or build sources that
call each other, such as expressions and scans, as one target.

## Partial support

The emulator is useful because it fails locally where BigQuery would behave differently, so a
statement, clause, option or API field is supported only as far as clients cannot tell it apart
from BigQuery. Decide each one by what a client can observe:

- **Accept and ignore** it only when neither results nor metadata depend on it, such as a
  request's `timeoutMs`, since jobs run synchronously.
- **Store and return** it when it is only metadata, such as a table's description or labels;
  storing it is the whole implementation, and DDL and the REST API should agree.
- **Reject** anything that changes behavior, such as a required partition filter or an
  expiration, as unsupported until it is implemented. Never accept a form and silently drop what
  it does, even to let more queries run.

A feature that rejects some of its forms is Partial in the probes under `tools/`, not Supported.
Deliberate exceptions, kept for convenience, are listed in the README's Compatibility section.

## Implementing functions

Compatibility comes first; prefer implementations with less code and fewer special cases. Leave
out an overload, argument type or form whose semantics would cost too much rather than approximate
it; a call no rule or handler matches stays unsupported. Existing registrations are not evidence
of compatibility, and a successful probe alone does not establish it; revisit one only when the
current task requires it. Keep conditional expressions such as IF and COALESCE as SQL expressions
so they can short-circuit.

For other scalar functions, choose the backend by what the function does, not by which one to try
first:

- **DuckDB**: what the optimizer must see, such as operators, comparisons and date and time
  arithmetic, truncation and extraction, which filters, joins and partition pruning depend on;
  functions whose result the types' arithmetic or a specification fixes, such as ABS, ROUND,
  hashes and hex or base64 encoding; what depends on the engine, such as RAND and
  CURRENT_TIMESTAMP; and ARRAY and STRUCT operations. Check each overload against BigQuery at
  boundaries such as overflow, ranges, NaN or negative arguments. Names can match while semantics
  do not: DuckDB's `concat` skips NULL where BigQuery's CONCAT returns NULL.
- **GoogleSQL's implementation**: semantics engines interpret differently, such as Unicode case
  mapping, normalization and whitespace, regular expressions, formatting and parsing text, JSON,
  and transcendental functions such as SIN and EXP. Use it for the function's semantics, including
  input validation, result ranges and errors, rather than reproducing them in DuckDB SQL.
  GoogleSQL can differ from BigQuery too; check its behavior, and adapt a known difference in the
  backend function or the rule that calls it, with a case that shows BigQuery's answer. Prefer
  speeding up a slow backend function; a move to DuckDB requires evidence of compatibility for
  the affected overloads, not speed alone.

Choose the backend by these categories; then, for an overload assigned to DuckDB, close gaps
with guards and errors, such as for a negative argument or an out-of-range result; if compatibility
requires rewriting the computation, use GoogleSQL. A guardable boundary difference alone does not
override the GoogleSQL category, including for SIN and EXP. When an overload fits both categories,
or neither clearly, use GoogleSQL.

`src/translator/functions.cc` registers each function under one of these spellings; its rules may
still spell overloads with different backends, such as a DuckDB function for STRING and
GoogleSQL's for BYTES:

- **As is** (`PlainFunctions`) or **renamed** (`FunctionNames`): a DuckDB function with the same
  semantics.
- **A template** (`TemplateRules`): a DuckDB function plus guards and errors, or a small fixed
  difference such as argument order, a default or a date part.
- **A backend rule** (`BackendRules`, registered in `src/backend_functions/`): a call to
  GoogleSQL's implementation.
- **A handler** (`Handlers`, `src/translator/handlers.cc`): when the spelling depends on more of
  the resolved AST than a rule sees, such as a variable number of arguments or a literal pattern
  or path. The handler builds the call and leaves the semantics to the backend chosen above; do
  not reimplement them in generated SQL, such as by walking BYTES as hex digits.

Aggregate and window functions map to DuckDB ones through `AggregateRule`. A function its fields
cannot express stays unsupported unless it is common enough to justify a custom aggregate.

Check each overload against BigQuery's documentation for:

- **NULL**: NULL arguments, NULL array elements, and empty strings, bytes and arrays.
- **Types**: the DuckDB result type matches the type GoogleSQL resolves, for every argument type
  the function accepts (STRING or BYTES; INT64, NUMERIC, BIGNUMERIC or FLOAT64).
- **Errors**: where BigQuery fails, the emulator fails too, rather than returning NULL, an infinity
  or a clamped value. Propagate GoogleSQL's errors; in generated SQL, use `!n` or `Raise()` so
  `SAFE.` turns the function's errors into NULL. Where BigQuery's message is known, use it, even
  over GoogleSQL's.
- **Evaluation**: preserve short-circuit evaluation in conditional expressions. A handler that
  uses an argument more than once binds it once, as templates do.

Cover each of these that applies with a case in `tests/e2e/goclient/testdata/scalars/`.

## Tests and probes

Each layer checks one thing; write a form in the layer that checks it, and never copy SQL and its
expected result from one layer to another.

- **End-to-end tests** (`tests/e2e`, `just e2e`) check what a client sees against BigQuery, and
  come first. [runn](https://github.com/k1LoW/runn) drives the emulator with `bq` and the Go
  client. A query that returns one value goes in `tests/e2e/goclient/testdata/scalars/`. Use `bq`
  only for what depends on it, such as its flags and output formats. Runbooks run concurrently, so
  each uses datasets of its own, and a project of its own where it lists a whole project. REST
  details no client surfaces are checked with runn's HTTP runner in `tests/e2e/rest_*.yml`.
- **Probes** (`tools/`, `just docs`) check only that a form translates and runs, and generate
  `docs/api.md`, `docs/sql.md` and `docs/functions.md`; never edit those by hand, and commit them
  regenerated. `//:function_probe` builds calls from GoogleSQL's signatures: write a hint only for
  a form it cannot build, and extend it rather than writing hints for a whole class of forms.
- **GoogleSQL's compliance tests** (`just compliance`) check query results broadly against
  GoogleSQL's reference implementation, which is not BigQuery. CI fails on a statement that
  fails unless `tests/compliance/known_errors.textproto`, or the file for a platform it fails on
  only, lists it by name, and on a listed one that passes, so a change that fixes a statement
  removes it from the list and one that newly fails a statement, such as by supporting a function,
  lists it with the reason; `just compliance-summary` names both. The bugs they found are `!>`
  cases of `scalars/known_bugs.txt`, each to be fixed and turned into a `=>` case.
- **C++ tests** (`just test`) check what clients cannot observe, such as translation edge cases,
  or runn cannot drive, such as many concurrent connections.

### Expected values

Take each expected value from BigQuery's documentation, from GoogleSQL's compliance test data
where the reference implementation agrees with BigQuery, or from BigQuery itself. Never copy the
emulator's output. Agents do not run queries on BigQuery: a case no source settles goes in
`tests/e2e/goclient/testdata/unverified/*.txt`, split by feature, whose answers maintainers fill in with
`just bigquery-answers`. `just reference` shows what the reference implementation answers, as a
lead for what to check, never as an expected value.

Until an independent source settles a case, do not rely on a guessed answer to support the
behavior that case covers; the rest of the function can still be supported. An unverified case
alone does not require removing existing support.

## Before committing

Run `just fmt` and `just check`.
