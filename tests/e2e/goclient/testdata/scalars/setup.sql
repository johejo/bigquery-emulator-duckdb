-- The table that the cases in *.txt read as `t` from their default dataset, and
-- a dataset whose default rounding mode is ROUND_HALF_EVEN, which cases create tables in.
CREATE SCHEMA IF NOT EXISTS scalar_graph;
CREATE TABLE scalar_graph.Person (id INT64, name STRING);
INSERT scalar_graph.Person VALUES (1, 'Alex'), (2, 'Dana'), (3, 'Lee');
CREATE TABLE scalar_graph.Knows (id INT64, src INT64, dst INT64);
INSERT scalar_graph.Knows VALUES (10, 1, 2), (11, 2, 1), (12, 1, 1), (13, 1, 2);
CREATE TABLE scalar_graph.Account (id INT64, kind STRING);
INSERT scalar_graph.Account VALUES (7, 'checking');
CREATE TABLE scalar_graph.Owns (id INT64, account_id INT64);
INSERT scalar_graph.Owns VALUES (1, 7);
CREATE PROPERTY GRAPH scalar_graph.FinGraph
NODE TABLES (scalar_graph.Person KEY (id), scalar_graph.Account KEY (id))
EDGE TABLES (
  scalar_graph.Knows KEY (id) SOURCE KEY (src) REFERENCES Person (id)
    DESTINATION KEY (dst) REFERENCES Person (id),
  scalar_graph.Owns KEY (id, account_id) SOURCE KEY (id) REFERENCES Person (id)
    DESTINATION KEY (account_id) REFERENCES Account (id)
);
CREATE PROPERTY GRAPH scalar_graph.HiddenKeys
NODE TABLES (scalar_graph.Person KEY (id) NO PROPERTIES)
EDGE TABLES (scalar_graph.Knows KEY (id) SOURCE KEY (src) REFERENCES Person (id)
  DESTINATION KEY (dst) REFERENCES Person (id) NO PROPERTIES);
CREATE TABLE scalar_graph.Compound (tenant STRING, id INT64);
INSERT scalar_graph.Compound VALUES ('a', 1), ('a', 2), ('b', 1);
CREATE TABLE scalar_graph.CompoundEdge (id STRING, src_tenant STRING, src_id INT64, dst_tenant STRING, dst_id INT64);
INSERT scalar_graph.CompoundEdge VALUES ('x', 'a', 1, 'a', 2), ('y', 'a', 1, 'b', 1), ('loop', 'b', 1, 'b', 1);
CREATE PROPERTY GRAPH scalar_graph.CompoundGraph
NODE TABLES (scalar_graph.Compound KEY (tenant, id))
EDGE TABLES (scalar_graph.CompoundEdge KEY (id)
  SOURCE KEY (src_tenant, src_id) REFERENCES Compound (tenant, id)
  DESTINATION KEY (dst_tenant, dst_id) REFERENCES Compound (tenant, id)
  PROPERTIES (id AS edge_id, src_tenant AS origin, dst_tenant AS target));
CREATE PROPERTY GRAPH scalar_graph.AliasedNodes
NODE TABLES (scalar_graph.Person AS Authors KEY (id) LABEL Author,
  scalar_graph.Person AS Readers KEY (id) LABEL Reader)
EDGE TABLES (scalar_graph.Knows KEY (id) SOURCE KEY (src) REFERENCES Authors (id)
  DESTINATION KEY (dst) REFERENCES Readers (id));

-- Person, account, ownership and transfer rows from BigQuery's documented FinGraph example,
-- without its TIMESTAMP columns.
-- third_party/bigquery_docs/reference/standard-sql/graph-schema-statements.md
CREATE TABLE scalar_graph.TransferAccount (id INT64, is_blocked BOOL);
INSERT scalar_graph.TransferAccount VALUES (7, FALSE), (16, TRUE), (20, FALSE);
CREATE TABLE scalar_graph.Transfer (id INT64, to_id INT64, amount INT64);
INSERT scalar_graph.Transfer VALUES (7, 16, 300), (7, 16, 100), (16, 20, 300), (20, 7, 500), (20, 16, 200);
CREATE TABLE scalar_graph.PersonOwnAccount (id INT64, account_id INT64);
INSERT scalar_graph.PersonOwnAccount VALUES (1, 7), (2, 20), (3, 16);
CREATE PROPERTY GRAPH scalar_graph.TransferGraph
NODE TABLES (scalar_graph.TransferAccount AS Account KEY (id), scalar_graph.Person KEY (id))
EDGE TABLES (scalar_graph.Transfer KEY (id, to_id, amount) SOURCE KEY (id) REFERENCES Account (id)
  DESTINATION KEY (to_id) REFERENCES Account (id) LABEL Transfers,
  scalar_graph.PersonOwnAccount KEY (id, account_id) SOURCE KEY (id) REFERENCES Person (id)
  DESTINATION KEY (account_id) REFERENCES Account (id) LABEL Owns);

CREATE SCHEMA IF NOT EXISTS scalars;
CREATE SCHEMA IF NOT EXISTS scalars_rounding_even OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN');
CREATE OR REPLACE TABLE scalars.t (a INT64, b STRING, raw BYTES);
INSERT INTO scalars.t VALUES (3, 'あ', FROM_HEX('00ff')), (1, 'x', FROM_HEX('61')),
  (2, 'yy', NULL), (NULL, NULL, NULL);
