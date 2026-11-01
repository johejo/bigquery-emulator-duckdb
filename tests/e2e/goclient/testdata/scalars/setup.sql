-- The table that the cases in *.txt read as `t` from their default dataset, and
-- a dataset whose default rounding mode is ROUND_HALF_EVEN, which cases create tables in.
CREATE SCHEMA IF NOT EXISTS scalars;
CREATE SCHEMA IF NOT EXISTS scalars_rounding_even OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN');
CREATE OR REPLACE TABLE scalars.t (a INT64, b STRING, raw BYTES);
INSERT INTO scalars.t VALUES (3, 'あ', FROM_HEX('00ff')), (1, 'x', FROM_HEX('61')),
  (2, 'yy', NULL), (NULL, NULL, NULL);

-- A collated column and a view over it, which keeps its collation.
-- third_party/bigquery_docs/reference/standard-sql/collation-concepts.md
CREATE OR REPLACE TABLE scalars.collated (c STRING COLLATE 'und:ci');
INSERT INTO scalars.collated VALUES ('Apple'), ('apple'), ('Banana');
CREATE OR REPLACE VIEW scalars.collated_view AS SELECT c FROM scalars.collated;
