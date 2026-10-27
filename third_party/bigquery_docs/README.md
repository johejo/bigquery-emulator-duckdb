# BigQuery documentation snapshots

This directory holds Markdown snapshots of BigQuery documentation cited by this repository.
Use saved pages for specification lookups instead of fetching their upstream URLs. This is a
snapshot, not a guarantee that BigQuery's current behavior agrees with the documentation.

## Adding and updating citations

Write a new official BigQuery documentation URL in a comment or document, then run:

```sh
just bigquery-docs-sync
just bigquery-docs-check
```

### Authentication

Enable the [Developer Knowledge API](https://developers.google.com/knowledge/quickstart) in the
project used for the requests, and grant the authenticated principal the required permissions,
including permission to use the quota project (`serviceusage.services.use`). The tool uses its
REST v1 `documents.get` method, not BigQuery itself. It does not run BigQuery queries or retrieve
documents by approximate search.

By default, acquisition uses Application Default Credentials (ADC). For local development:

```sh
gcloud auth application-default login
gcloud auth application-default set-quota-project YOUR_PROJECT
just bigquery-docs-sync
```

`gcloud auth login` and ADC are separate configurations. The Google authentication library
handles credential discovery, OAuth token refresh, and the `X-Goog-User-Project` quota header.
It also supports ADC supplied through `GOOGLE_APPLICATION_CREDENTIALS` or an attached service
account. `GOOGLE_CLOUD_QUOTA_PROJECT` overrides the quota project from the credentials when set.
Do not copy the output of `gcloud auth print-access-token` into an API-key variable.

Alternatively, set an API key; an explicit key takes precedence over ADC:

```sh
export DEVELOPERKNOWLEDGE_API_KEY='your-api-key'
just bigquery-docs-sync
```

Credentials are initialized only when a page must actually be fetched. `scan`, `check`, `render`,
and `sync` with all referenced pages already saved do not discover ADC or contact token endpoints.
Credentials and tokens are never stored in the manifest or snapshots. The tool does not invoke
`gcloud` itself.

Commit the snapshot, `manifest.json`, and localized citations together. `sync` fetches only
missing pages and rewrites citations only after saving their content. Pages that fail to fetch
remain remote; successful pages are kept, and the command reports failures with a nonzero exit.
Existing snapshots are not fetched again. Use `just bigquery-docs-refresh` to explicitly update
all saved and newly referenced pages. Unchanged responses do not change files; unreferenced
snapshots are reported but never deleted automatically.

`just bigquery-docs-scan` lists pages and referring files. `just bigquery-docs-check` checks page
availability, snapshot hashes, local paths, unlocalized citations, and unclassified URLs without
network access. Local citations link to whole files, without section anchors: the API's Markdown
does not reliably preserve the website's anchor IDs. `sync` removes anchors from existing local
citations as well as new ones, and `check` rejects local citations with anchors. Link labels and
surrounding explanations are preserved so readers can search for the relevant section. Snapshot
contents and the machine-readable function index retain their original links and anchors.

The scanner reads Git-tracked regular UTF-8 text files, excluding `third_party/` and the tool's
own source. It recognizes Markdown, source comments, scalar case comments, and top-level YAML
scenario descriptions as citations. The relative page names in `tools/bigquery_functions.txt`
are also included. New files must be added to Git before scanning. Other URLs must be classified
explicitly in `tools/bigquerydocs/exceptions.json`; exceptions need a file, matching text, and a
reason. Client-visible error messages, recorded answers, and operational upstream URLs must not
be rewritten as documentation links.

Markdown citations are relative to their document; source comments use repository-root-relative
paths. Embedded restprobe preambles use paths relative to their rendered documents in `docs/`.
`just docs` and `just docs-check` localize generated reports against the saved manifest, so
regeneration does not restore external citations. Missing snapshots leave generated URLs intact
and are caught by `bigquery-docs-check`.

## Provenance and licensing

`manifest.json` records each page's official source URL, API resource name, title, upstream update
time when available, local filename, and SHA-256 hash. Page content is stored as returned by the
API, without reformatting, summarization, or recursive retrieval of linked documents. Original
URLs remain in the manifest for attribution and explicit updates, not as instructions to browse.

Google Cloud documentation is generally licensed under
[Creative Commons Attribution 4.0](https://creativecommons.org/licenses/by/4.0/), and code samples
under [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0), except as otherwise noted.
Attribution: Google, the documentation titles and source URLs recorded in `manifest.json`.
See [Google's site policies](https://developers.google.com/site-policies) and each source page
for applicable notices and exceptions. Review page-specific notices when adding snapshots; the
tool does not determine their licensing automatically. Markdown is supplied through the Developer
Knowledge API rather than copied from the HTML presentation.

An empty manifest means initial acquisition has not run yet; it requires ADC or an API key.
No placeholder pages or links to nonexistent snapshots are generated. Until that initial
acquisition is complete, the offline citation check reports missing snapshots and remote citations;
it is a separate recipe, not yet a dependency of the repository-wide `just check`.
