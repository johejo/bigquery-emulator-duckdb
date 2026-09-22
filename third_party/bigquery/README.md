# BigQuery discovery document

`discovery.json` is the BigQuery v2 REST discovery document bundled with the `bq` command-line
tool (`platform/bq/discovery_next/bigquery.json` in the Google Cloud SDK). `bq` fetches the
discovery document from `<api>/$discovery/rest?version=v2` whenever `--api` points at a
non-Google endpoint, so the emulator serves this copy with `rootUrl` and `baseUrl` rewritten to
its own address.
