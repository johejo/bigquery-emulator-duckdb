# AGENTS.md

C++20 built with Bazel; prefer the standard library. `nix develop` provides the toolchain,
including `bq`. Run `just` recipes (see the `Justfile`) rather than raw commands.

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
it; a call no rule or handler matches stays unsupported. `src/translator/functions.cc` registers
each function under one implementation; pick the first that fits. Existing registrations are not
evidence of compatibility.

1. **DuckDB as is** (`PlainFunctions`, `FunctionNames`): only when DuckDB agrees on every input,
   including NULL, NaN and infinities, overflow and errors. Names can match while semantics do
   not: DuckDB's `concat` skips NULL where BigQuery's `CONCAT` returns NULL.
2. **GoogleSQL's implementation** (`BackendRules`, registered in `src/backend_functions/`): the
   default for regular expressions, STRING and BYTES handling, JSON, and parsing or formatting
   text, where engines differ in edge cases. If a DuckDB spelling needs tricks such as walking
   BYTES as hex digits, use this instead.
3. **A template** (`TemplateRules`): a DuckDB function plus a small fixed difference, such as
   argument order, a default, a date part, or an error DuckDB lacks.
4. **A handler** (`Handlers`, `src/translator/function.cc`): when the spelling depends on more of
   the resolved AST than a template sees, such as a variable number of arguments or a literal
   pattern or path. A handler that reimplements the function's semantics belongs in GoogleSQL's
   implementation instead.

Aggregate and window functions map to DuckDB ones through `AggregateRule`. A function its fields
cannot express stays unsupported unless it is common enough to justify a custom aggregate.

Check each overload against BigQuery's documentation for:

- **NULL**: NULL arguments, NULL array elements, and empty strings, bytes and arrays.
- **Types**: the DuckDB result type matches the type GoogleSQL resolves, for every argument type
  the function accepts (STRING or BYTES; INT64, NUMERIC, BIGNUMERIC or FLOAT64).
- **Errors**: where BigQuery fails, the emulator fails too, rather than returning NULL, an infinity
  or a clamped value; raise it with `!n` or `Raise()` so `SAFE.` turns it into NULL. Use
  BigQuery's message where it is known.
- **Evaluation**: a handler that uses an argument more than once binds it once, as templates do.

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
  GoogleSQL's reference implementation, which is not BigQuery. Failures are listed in
  `tests/compliance/known_errors.textproto`; the bugs they found are `!>` cases of
  `scalars/known_bugs.txt`, each to be fixed and turned into a `=>` case.
- **C++ tests** (`just test`) check what clients cannot observe, such as translation edge cases,
  or runn cannot drive, such as many concurrent connections.

### Expected values

Take each expected value from BigQuery's documentation, from GoogleSQL's compliance test data
where the reference implementation agrees with BigQuery, or from BigQuery itself. Never copy the
emulator's output. Agents do not run queries on BigQuery: a case no source settles goes in
`tests/e2e/goclient/testdata/unverified.txt`, whose answers maintainers fill in with
`just bigquery-answers`. `just reference` shows what the reference implementation answers, as a
lead for what to check, never as an expected value.

## Before committing

Run `just fmt` and `just check`.
