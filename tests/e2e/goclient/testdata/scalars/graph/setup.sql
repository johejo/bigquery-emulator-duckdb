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

-- Aliases of an edge table have distinct element identities, just like node aliases.
-- third_party/bigquery_docs/reference/standard-sql/graph-schema-statements.md
CREATE PROPERTY GRAPH scalar_graph.AliasedEdges
NODE TABLES (scalar_graph.Person KEY (id))
EDGE TABLES (
  scalar_graph.Knows AS FirstEdges KEY (id) SOURCE KEY (src) REFERENCES Person (id)
    DESTINATION KEY (dst) REFERENCES Person (id) LABEL FirstKnows,
  scalar_graph.Knows AS SecondEdges KEY (id) SOURCE KEY (src) REFERENCES Person (id)
    DESTINATION KEY (dst) REFERENCES Person (id) LABEL SecondKnows
);

-- Labels expose different property sets on the same element. Entity and Relation are
-- shared across element tables; overlapping label unions must not duplicate elements.
-- third_party/bigquery_docs/reference/standard-sql/graph-schema-statements.md
CREATE PROPERTY GRAPH scalar_graph.MultiLabels
NODE TABLES (
  scalar_graph.Person KEY (id)
    LABEL Person PROPERTIES (id, name)
    LABEL Entity PROPERTIES (id)
    LABEL Named PROPERTIES (name),
  scalar_graph.Account KEY (id)
    LABEL Account PROPERTIES (id, kind)
    LABEL entity PROPERTIES (ID)
)
EDGE TABLES (
  scalar_graph.Knows KEY (id) SOURCE KEY (src) REFERENCES Person (id)
    DESTINATION KEY (dst) REFERENCES Person (id)
    LABEL Knows PROPERTIES (id) LABEL Relation NO PROPERTIES,
  scalar_graph.Owns KEY (id, account_id) SOURCE KEY (id) REFERENCES Person (id)
    DESTINATION KEY (account_id) REFERENCES Account (id)
    LABEL Owns PROPERTIES (account_id) LABEL Relation NO PROPERTIES
);

-- Properties defined by expressions over the element table's columns.
-- third_party/bigquery_docs/reference/standard-sql/graph-schema-statements.md
CREATE PROPERTY GRAPH scalar_graph.ComputedProperties
NODE TABLES (
  scalar_graph.Person KEY (id)
    LABEL Person PROPERTIES (id, 1000 + id AS code, UPPER(name) AS upper_name, 'person' AS kind),
  scalar_graph.Account KEY (id)
    LABEL Account PROPERTIES (id, CONCAT(kind, '!') AS kind)
)
EDGE TABLES (
  scalar_graph.Knows KEY (id) SOURCE KEY (src) REFERENCES Person (id)
    DESTINATION KEY (dst) REFERENCES Person (id)
    LABEL Knows PROPERTIES (id, IF(src = dst, NULL, dst - src) AS delta)
);

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
-- BigQuery's documented FinGraph example as written, apart from the dataset name and
-- CREATE PROPERTY GRAPH without OR REPLACE. Its element tables take their tables' primary keys.
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
    scalar_fingraph.Account,
    scalar_fingraph.Person
  )
  EDGE TABLES (
    scalar_fingraph.PersonOwnAccount
      SOURCE KEY (id) REFERENCES Person (id)
      DESTINATION KEY (account_id) REFERENCES Account (id)
      LABEL Owns,
    scalar_fingraph.AccountTransferAccount
      SOURCE KEY (id) REFERENCES Account (id)
      DESTINATION KEY (to_id) REFERENCES Account (id)
      LABEL Transfers
  );

-- GRAPH_EXPAND's documented StoreGraph, without its measure.
-- third_party/bigquery_docs/reference/standard-sql/graph-sql-queries.md
CREATE TABLE scalar_graph.Locations (id INT64 PRIMARY KEY NOT ENFORCED, name STRING, population INT64)
AS (SELECT 101 AS id, 'Anytown' AS name, 1000 AS population
  UNION ALL SELECT 102 AS id, 'Sometown' AS name, 500 AS population);
CREATE TABLE scalar_graph.Stores (name STRING PRIMARY KEY NOT ENFORCED,
  location_id INT64 REFERENCES scalar_graph.Locations(id) NOT ENFORCED)
AS (SELECT 'Store 1' AS name, 101 AS location_id UNION ALL SELECT 'Store 2' AS name, 101 AS location_id);
CREATE PROPERTY GRAPH scalar_graph.StoreGraph
  NODE TABLES (scalar_graph.Stores AS S, scalar_graph.Locations AS L PROPERTIES (id, name, population))
  EDGE TABLES (scalar_graph.Stores AS SL SOURCE KEY (location_id) REFERENCES L (id)
    DESTINATION KEY (name) REFERENCES S (name));
CREATE PROPERTY GRAPH scalar_graph.StoreGraphReversed
  NODE TABLES (scalar_graph.Stores AS S, scalar_graph.Locations AS L PROPERTIES (id, name, population))
  EDGE TABLES (scalar_graph.Stores AS SL SOURCE KEY (name) REFERENCES S (name)
    DESTINATION KEY (location_id) REFERENCES L (id));
CREATE TABLE scalar_graph.SizedStores (name STRING PRIMARY KEY NOT ENFORCED, location_id INT64,
  size INT64)
AS (SELECT 'Store 1' AS name, 101 AS location_id, 3 AS size UNION ALL SELECT 'Store 2', 101, 3);
CREATE PROPERTY GRAPH scalar_graph.SizedStoreGraph
  NODE TABLES (scalar_graph.SizedStores AS S, scalar_graph.Locations AS L PROPERTIES (id, name, population))
  EDGE TABLES (scalar_graph.SizedStores AS SL SOURCE KEY (location_id) REFERENCES L (id)
    DESTINATION KEY (name) REFERENCES S (name));
CREATE PROPERTY GRAPH scalar_graph.StoreLocations
  NODE TABLES (scalar_graph.Stores AS S, scalar_graph.Locations AS L);
CREATE PROPERTY GRAPH scalar_graph.LabeledStores
  NODE TABLES (scalar_graph.Stores AS S LABEL Store);
-- An edge table with a property of its own.
CREATE TABLE scalar_graph.Places (id INT64 PRIMARY KEY NOT ENFORCED, name STRING)
AS SELECT 1 AS id, 'p' AS name UNION ALL SELECT 2, 'q';
CREATE TABLE scalar_graph.Shops (id INT64 PRIMARY KEY NOT ENFORCED, place_id INT64,
  other_place_id INT64, size INT64)
AS SELECT 1 AS id, 1 AS place_id, 2 AS other_place_id, 3 AS size UNION ALL SELECT 2, NULL, NULL, 4;
CREATE PROPERTY GRAPH scalar_graph.ShopPlaces
  NODE TABLES (scalar_graph.Shops AS S, scalar_graph.Places AS P)
  EDGE TABLES (scalar_graph.Shops AS SP SOURCE KEY (id) REFERENCES S (id)
    DESTINATION KEY (place_id) REFERENCES P (id) PROPERTIES (size AS edge_size));
-- Unflattened outputs from a non-inlined edge: a match, a match with a NULL property,
-- a NULL join key and an orphan key. The edge stays non-inlined without a custom property
-- because size is not a reference column.
CREATE TABLE scalar_graph.NestedPlaces (id INT64 PRIMARY KEY NOT ENFORCED, name STRING)
AS SELECT 1 AS id, 'p' AS name UNION ALL SELECT 2, NULL;
CREATE TABLE scalar_graph.NestedShops (id INT64 PRIMARY KEY NOT ENFORCED, place_id INT64, size INT64)
AS SELECT 1 AS id, 1 AS place_id, 3 AS size
UNION ALL SELECT 2, 2, 4
UNION ALL SELECT 3, NULL, 5;
CREATE PROPERTY GRAPH scalar_graph.NestedShopPlaces
  NODE TABLES (scalar_graph.NestedShops AS S, scalar_graph.NestedPlaces AS P)
  EDGE TABLES (scalar_graph.NestedShops AS SP SOURCE KEY (id) REFERENCES S (id)
    DESTINATION KEY (place_id) REFERENCES P (id) PROPERTIES (size AS edge_size));
CREATE PROPERTY GRAPH scalar_graph.NestedShopPlacesDefaultEdge
  NODE TABLES (scalar_graph.NestedShops AS S, scalar_graph.NestedPlaces AS P)
  EDGE TABLES (scalar_graph.NestedShops AS SP SOURCE KEY (id) REFERENCES S (id)
    DESTINATION KEY (place_id) REFERENCES P (id));
-- The emulator rejects an existing orphan at CREATE PROPERTY GRAPH time. Add it afterward
-- so the expansion starts with the same input rows as the recorded BigQuery query.
INSERT scalar_graph.NestedShops VALUES (4, 99, 6);
-- Endpoints that reference columns other than the node key: each row of KindTransfers relates
-- every checking account to every savings account, and the other way round.
CREATE TABLE scalar_graph.KindAccounts AS
SELECT 1 AS id, 'checking' AS kind UNION ALL SELECT 2, 'savings' UNION ALL SELECT 3, 'checking';
CREATE TABLE scalar_graph.KindTransfers AS
SELECT 'checking' AS src_kind, 'savings' AS dst_kind UNION ALL SELECT 'savings', 'checking';
CREATE PROPERTY GRAPH scalar_graph.KindGraph
  NODE TABLES (scalar_graph.KindAccounts AS Account KEY (id))
  EDGE TABLES (scalar_graph.KindTransfers AS CanTransfer KEY (src_kind, dst_kind)
    SOURCE KEY (src_kind) REFERENCES Account (kind)
    DESTINATION KEY (dst_kind) REFERENCES Account (kind));
-- Edge tables with their source node's table and properties, which GRAPH_EXPAND joins between
-- their node tables, in the order the graph defines them.
CREATE PROPERTY GRAPH scalar_graph.ShopsNear
  NODE TABLES (scalar_graph.Shops AS S, scalar_graph.Places AS Near)
  EDGE TABLES (scalar_graph.Shops AS SN SOURCE KEY (id) REFERENCES S (id)
    DESTINATION KEY (place_id) REFERENCES Near (id));
CREATE PROPERTY GRAPH scalar_graph.ShopsNearFar
  NODE TABLES (scalar_graph.Shops AS S, scalar_graph.Places AS Near, scalar_graph.Places AS Far)
  EDGE TABLES (scalar_graph.Shops AS SN SOURCE KEY (id) REFERENCES S (id)
      DESTINATION KEY (place_id) REFERENCES Near (id),
    scalar_graph.Shops AS SF SOURCE KEY (id) REFERENCES S (id)
      DESTINATION KEY (other_place_id) REFERENCES Far (id));
CREATE PROPERTY GRAPH scalar_graph.ShopsFarNear
  NODE TABLES (scalar_graph.Shops AS S, scalar_graph.Places AS Far, scalar_graph.Places AS Near)
  EDGE TABLES (scalar_graph.Shops AS SF SOURCE KEY (id) REFERENCES S (id)
      DESTINATION KEY (other_place_id) REFERENCES Far (id),
    scalar_graph.Shops AS SN SOURCE KEY (id) REFERENCES S (id)
      DESTINATION KEY (place_id) REFERENCES Near (id));
CREATE PROPERTY GRAPH scalar_graph.ShopsFarNearEdges
  NODE TABLES (scalar_graph.Shops AS S, scalar_graph.Places AS Far, scalar_graph.Places AS Near)
  EDGE TABLES (scalar_graph.Shops AS SN SOURCE KEY (id) REFERENCES S (id)
      DESTINATION KEY (place_id) REFERENCES Near (id),
    scalar_graph.Shops AS SF SOURCE KEY (id) REFERENCES S (id)
      DESTINATION KEY (other_place_id) REFERENCES Far (id));
-- An edge that references its node table's table as its destination, as the documented
-- StoreGraph's edge does, over a table with other columns.
CREATE PROPERTY GRAPH scalar_graph.ShopsNearInlined
  NODE TABLES (scalar_graph.Shops AS S, scalar_graph.Places AS Near)
  EDGE TABLES (scalar_graph.Shops AS NS SOURCE KEY (place_id) REFERENCES Near (id)
    DESTINATION KEY (id) REFERENCES S (id));
-- An edge whose key is fewer columns than those that reference its source node.
CREATE PROPERTY GRAPH scalar_graph.ShopSizes
  NODE TABLES (scalar_graph.Shops AS S, scalar_graph.Places AS P)
  EDGE TABLES (scalar_graph.Shops AS SP KEY (id) SOURCE KEY (id, size) REFERENCES S (id, size)
    DESTINATION KEY (place_id) REFERENCES P (id) PROPERTIES (size AS edge_size));

-- The first branch has a descendant; the second branch survives when the first is missing.
CREATE TABLE scalar_graph.ExpandRoots (id INT64 PRIMARY KEY NOT ENFORCED)
AS SELECT id FROM UNNEST([1, 2, 3]) AS id;
CREATE TABLE scalar_graph.ExpandBranches (id INT64 PRIMARY KEY NOT ENFORCED, leaf_id INT64)
AS SELECT 10 AS id, 100 AS leaf_id UNION ALL SELECT 20, NULL;
CREATE TABLE scalar_graph.ExpandLeaves (id INT64 PRIMARY KEY NOT ENFORCED, name STRING)
AS SELECT 100 AS id, 'leaf' AS name;
CREATE TABLE scalar_graph.ExpandSiblings (id INT64 PRIMARY KEY NOT ENFORCED, name STRING)
AS SELECT 200 AS id, 'sibling' AS name;
CREATE TABLE scalar_graph.ExpandRootBranch (id INT64 PRIMARY KEY NOT ENFORCED, target_id INT64, score INT64)
AS SELECT 1 AS id, 10 AS target_id, 11 AS score UNION ALL SELECT 2, 20, 22;
CREATE TABLE scalar_graph.ExpandRootSibling (id INT64 PRIMARY KEY NOT ENFORCED, target_id INT64, score INT64)
AS SELECT id, 200 AS target_id, id * 2 AS score FROM UNNEST([1, 2, 3]) AS id;
CREATE PROPERTY GRAPH scalar_graph.ExpandTree
  NODE TABLES (scalar_graph.ExpandRoots AS A, scalar_graph.ExpandBranches AS B,
    scalar_graph.ExpandLeaves AS C, scalar_graph.ExpandSiblings AS D)
  EDGE TABLES (
    scalar_graph.ExpandRootBranch AS AB SOURCE KEY (id) REFERENCES A (id)
      DESTINATION KEY (target_id) REFERENCES B (id) PROPERTIES (score AS branch_score),
    scalar_graph.ExpandRootSibling AS AD SOURCE KEY (id) REFERENCES A (id)
      DESTINATION KEY (target_id) REFERENCES D (id) PROPERTIES (score AS sibling_score),
    scalar_graph.ExpandBranches AS BC SOURCE KEY (id) REFERENCES B (id)
      DESTINATION KEY (leaf_id) REFERENCES C (id));
CREATE TABLE scalar_graph.MeasureCollege (college_id INT64 PRIMARY KEY NOT ENFORCED, college_name STRING)
AS SELECT * FROM UNNEST([STRUCT(101 AS college_id, 'College of Engineering' AS college_name),
  (102, 'College of Arts')]);
CREATE TABLE scalar_graph.MeasureDepartment (dept_id INT64 PRIMARY KEY NOT ENFORCED, dept_name STRING,
  college_id INT64, budget FLOAT64)
AS SELECT * FROM UNNEST([
  STRUCT(1001 AS dept_id, 'Computer Science' AS dept_name, 101 AS college_id, 500000.0 AS budget),
  (1002, 'Mechanical Engineering', 101, 400000), (1003, 'Fine Arts', 102, 200000),
  (1004, 'Research', 101, 50000)]);
CREATE TABLE scalar_graph.MeasureCourse (course_id INT64 PRIMARY KEY NOT ENFORCED, course_name STRING,
  dept_id INT64, credits INT64)
AS SELECT * FROM UNNEST([
  STRUCT(1 AS course_id, 'Intro to CS' AS course_name, 1001 AS dept_id, 3 AS credits),
  (2, 'Data Structures', 1001, 4), (3, 'Thermodynamics', 1002, 3), (4, 'Oil Painting', 1003, 2)]);
CREATE TABLE scalar_graph.MeasureCourseWithOrphan (course_id INT64 PRIMARY KEY NOT ENFORCED,
  course_name STRING, dept_id INT64, credits INT64)
AS SELECT * FROM scalar_graph.MeasureCourse UNION ALL SELECT 5, 'Orphan', NULL, 1;
CREATE TABLE scalar_graph.MeasureInstructor (instructor_id INT64 PRIMARY KEY NOT ENFORCED,
  instructor_name STRING, dept_id INT64, salary FLOAT64)
AS SELECT * FROM UNNEST([
  STRUCT(501 AS instructor_id, 'Dr. Dana' AS instructor_name, 1001 AS dept_id, 120000.0 AS salary),
  (502, 'Dr. Lee', 1001, 110000), (503, 'Dr. Taylor', 1002, 115000), (504, 'Prof. Izumi', 1003, 90000)]);
CREATE PROPERTY GRAPH scalar_graph.SchoolGraph
  NODE TABLES (
    scalar_graph.MeasureCollege AS College KEY(college_id) PROPERTIES(college_id, college_name),
    scalar_graph.MeasureDepartment AS Department KEY(dept_id)
      PROPERTIES(dept_id, dept_name, college_id, budget,
        MEASURE(SUM(budget)) AS total_budget,
        MEASURE(SUM(budget) * 1.10) AS budget_with_overhead),
    scalar_graph.MeasureCourse AS Course KEY(course_id)
      PROPERTIES(course_id, course_name, credits, dept_id,
        MEASURE(AVG(credits)) AS avg_credits,
        MEASURE(SUM(credits)) AS total_credits,
        MEASURE(COUNT(course_id)) AS course_count,
        MEASURE(COUNT(DISTINCT CASE WHEN credits >= 3 THEN course_id ELSE NULL END)) AS major_course_count,
        MEASURE(SAFE_DIVIDE(AGG(total_credits), NULLIF(AGG(course_count), 0))) AS computed_avg_credits))
  EDGE TABLES (
    scalar_graph.MeasureDepartment AS CollegeDept
      SOURCE KEY (college_id) REFERENCES College (college_id)
      DESTINATION KEY (dept_id) REFERENCES Department (dept_id),
    scalar_graph.MeasureCourse AS DeptCourse
      SOURCE KEY (dept_id) REFERENCES Department (dept_id)
      DESTINATION KEY (course_id) REFERENCES Course (course_id));
CREATE PROPERTY GRAPH scalar_graph.SchoolGraphWithOrphan
  NODE TABLES (
    scalar_graph.MeasureDepartment AS Department KEY(dept_id)
      PROPERTIES(dept_id, dept_name, college_id, budget,
        COALESCE(budget, 0) AS budget_or_zero,
        MEASURE(AVG(COALESCE(budget, 0))) AS avg_budget_inline,
        MEASURE(AVG(budget_or_zero)) AS avg_budget_property),
    scalar_graph.MeasureCourseWithOrphan AS Course KEY(course_id)
      PROPERTIES(course_id, course_name, credits, dept_id))
  EDGE TABLES (
    scalar_graph.MeasureCourseWithOrphan AS DeptCourse
      SOURCE KEY (dept_id) REFERENCES Department (dept_id)
      DESTINATION KEY (course_id) REFERENCES Course (course_id));
CREATE PROPERTY GRAPH scalar_graph.SchoolGraphMultiRoot
  NODE TABLES (
    scalar_graph.MeasureCollege AS College KEY(college_id) PROPERTIES(college_id, college_name),
    scalar_graph.MeasureDepartment AS Department KEY(dept_id)
      PROPERTIES(dept_id, dept_name, college_id, budget, MEASURE(SUM(budget)) AS total_budget),
    scalar_graph.MeasureCourse AS Course KEY(course_id)
      PROPERTIES(course_id, course_name, credits, dept_id, MEASURE(COUNT(course_id)) AS course_count),
    scalar_graph.MeasureInstructor AS Instructor KEY(instructor_id)
      PROPERTIES(instructor_id, instructor_name, dept_id, salary,
        MEASURE(COUNT(instructor_id)) AS instructor_count, MEASURE(SUM(salary)) AS total_salary))
  EDGE TABLES (
    scalar_graph.MeasureDepartment AS CollegeDept
      SOURCE KEY (college_id) REFERENCES College (college_id)
      DESTINATION KEY (dept_id) REFERENCES Department (dept_id),
    scalar_graph.MeasureCourse AS DeptCourse
      SOURCE KEY (dept_id) REFERENCES Department (dept_id)
      DESTINATION KEY (course_id) REFERENCES Course (course_id),
    scalar_graph.MeasureInstructor AS DeptInstructor
      SOURCE KEY (dept_id) REFERENCES Department (dept_id)
      DESTINATION KEY (instructor_id) REFERENCES Instructor (instructor_id));
CREATE TABLE scalar_graph.MeasureAmounts (id INT64 PRIMARY KEY NOT ENFORCED, a INT64, b INT64)
AS SELECT 1 AS id, 100 AS a, 1 AS b UNION ALL SELECT 2, 100, 2 UNION ALL SELECT 3, NULL, 3;
CREATE TABLE scalar_graph.MeasureAmountRows (id INT64 PRIMARY KEY NOT ENFORCED, lid INT64)
AS SELECT id, CASE WHEN id <= 2 THEN 1 WHEN id = 3 THEN 2 WHEN id = 4 THEN 3 END AS lid
FROM UNNEST(GENERATE_ARRAY(1, 6)) AS id;
CREATE PROPERTY GRAPH scalar_graph.MeasureCountOneBeside
  NODE TABLES (scalar_graph.MeasureAmountRows AS S,
    scalar_graph.MeasureAmounts AS L PROPERTIES (id, a AS amount, b, MEASURE(SUM(amount) + COUNT(1)) AS m))
  EDGE TABLES (scalar_graph.MeasureAmountRows AS SL SOURCE KEY (lid) REFERENCES L (id)
    DESTINATION KEY (id) REFERENCES S (id));
CREATE PROPERTY GRAPH scalar_graph.MeasureSumOfConstantBeside
  NODE TABLES (scalar_graph.MeasureAmountRows AS S,
    scalar_graph.MeasureAmounts AS L PROPERTIES (id, a AS amount, b, MEASURE(SUM(amount) + SUM(1)) AS m))
  EDGE TABLES (scalar_graph.MeasureAmountRows AS SL SOURCE KEY (lid) REFERENCES L (id)
    DESTINATION KEY (id) REFERENCES S (id));
