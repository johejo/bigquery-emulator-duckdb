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
-- BigQuery's documented FinGraph example as written, apart from the dataset name, and
-- CREATE PROPERTY GRAPH without OR REPLACE and with the tables' primary keys as KEY clauses.
-- third_party/bigquery_docs/reference/standard-sql/graph-schema-statements.md
CREATE SCHEMA IF NOT EXISTS scalar_fingraph;
CREATE OR REPLACE TABLE scalar_fingraph.Person (
  id               INT64,
  name             STRING,
  birthday         TIMESTAMP,
  country          STRING,
  city             STRING,
  PRIMARY KEY (id) NOT ENFORCED
);
CREATE OR REPLACE TABLE scalar_fingraph.Account (
  id               INT64,
  create_time      TIMESTAMP,
  is_blocked       BOOL,
  nick_name        STRING,
  PRIMARY KEY (id) NOT ENFORCED
);
CREATE OR REPLACE TABLE scalar_fingraph.PersonOwnAccount (
  id               INT64 NOT NULL,
  account_id       INT64 NOT NULL,
  create_time      TIMESTAMP,
  PRIMARY KEY (id, account_id) NOT ENFORCED,
  FOREIGN KEY (id) REFERENCES scalar_fingraph.Person(id) NOT ENFORCED,
  FOREIGN KEY (account_id) REFERENCES scalar_fingraph.Account(id) NOT ENFORCED
);
CREATE OR REPLACE TABLE scalar_fingraph.AccountTransferAccount (
  id               INT64 NOT NULL,
  to_id            INT64 NOT NULL,
  amount           FLOAT64,
  create_time      TIMESTAMP NOT NULL,
  order_number     STRING,
  PRIMARY KEY (id, to_id, create_time) NOT ENFORCED,
  FOREIGN KEY (id) REFERENCES scalar_fingraph.Account(id) NOT ENFORCED,
  FOREIGN KEY (to_id) REFERENCES scalar_fingraph.Account(id) NOT ENFORCED
);
INSERT INTO scalar_fingraph.Account
  (id, create_time, is_blocked, nick_name)
VALUES
  (7,"2020-01-10 06:22:20.222",false,"Vacation Fund"),
  (16,"2020-01-27 17:55:09.206",true,"Vacation Fund"),
  (20,"2020-02-18 05:44:20.655",false,"Rainy Day Fund");
INSERT INTO scalar_fingraph.Person
  (id, name, birthday, country, city)
VALUES
  (1,"Alex","1991-12-21 00:00:00","Australia","Adelaide"),
  (2,"Dana","1980-10-31 00:00:00","Czech_Republic","Moravia"),
  (3,"Lee","1986-12-07 00:00:00","India","Kollam");
INSERT INTO scalar_fingraph.AccountTransferAccount
  (id, to_id, amount, create_time, order_number)
VALUES
  (7,16,300,"2020-08-29 15:28:58.647","304330008004315"),
  (7,16,100,"2020-10-04 16:55:05.342","304120005529714"),
  (16,20,300,"2020-09-25 02:36:14.926","103650009791820"),
  (20,7,500,"2020-10-04 16:55:05.342","304120005529714"),
  (20,16,200,"2020-10-17 03:59:40.247","302290001255747");
INSERT INTO scalar_fingraph.PersonOwnAccount
  (id, account_id, create_time)
VALUES
  (1,7,"2020-01-10 06:22:20.222"),
  (2,20,"2020-01-27 17:55:09.206"),
  (3,16,"2020-02-18 05:44:20.655");
CREATE PROPERTY GRAPH scalar_fingraph.FinGraph
  NODE TABLES (
    scalar_fingraph.Account KEY (id),
    scalar_fingraph.Person KEY (id)
  )
  EDGE TABLES (
    scalar_fingraph.PersonOwnAccount KEY (id, account_id)
      SOURCE KEY (id) REFERENCES Person (id)
      DESTINATION KEY (account_id) REFERENCES Account (id)
      LABEL Owns,
    scalar_fingraph.AccountTransferAccount KEY (id, to_id, create_time)
      SOURCE KEY (id) REFERENCES Account (id)
      DESTINATION KEY (to_id) REFERENCES Account (id)
      LABEL Transfers
  );

CREATE SCHEMA IF NOT EXISTS scalars;
CREATE SCHEMA IF NOT EXISTS scalars_rounding_even OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN');
CREATE OR REPLACE TABLE scalars.t (a INT64, b STRING, raw BYTES);
INSERT INTO scalars.t VALUES (3, 'あ', FROM_HEX('00ff')), (1, 'x', FROM_HEX('61')),
  (2, 'yy', NULL), (NULL, NULL, NULL);
