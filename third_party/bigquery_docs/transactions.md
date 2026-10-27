# Multi-statement transactions

BigQuery supports multi-statement transactions inside a single query, or across multiple queries when using sessions. A multi-statement transaction lets you perform mutating operations, such as inserting or deleting rows on one or more tables, and either commit or roll back the changes atomically.

Uses for multi-statement transactions include:

- Performing DML mutations on multiple tables as a single transaction. The tables can span multiple datasets or projects.
- Performing mutations on a single table in several stages, based on intermediate computations.

Transactions guarantee [ACID](https://en.wikipedia.org/wiki/ACID) properties and support snapshot isolation. During a transaction, all reads return a consistent snapshot of the tables referenced in the transaction. If a statement in a transaction modifies a table, the changes are visible to subsequent statements within the same transaction.

> **Note:** Reads from external data sources are not guaranteed to be consistent within a transaction if the underlying data source changes during the transaction.

## Transaction scope

A transaction must be contained in a single SQL query, except when in [`Session mode`](https://docs.cloud.google.com/bigquery/docs/sessions-intro) . A query can contain multiple transactions, but they cannot be nested. You can run [multi-statement transactions](https://docs.cloud.google.com/bigquery/docs/sessions-write-queries#multi_transactions) over multiple queries in a session.

To start a transaction, use the [`BEGIN TRANSACTION`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/procedural-language#begin_transaction) statement. The transaction ends when any of the following occur:

- The query executes a [`COMMIT TRANSACTION`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/procedural-language#commit_transaction) statement. This statement atomically commits all changes made inside the transaction.
- The query executes a [`ROLLBACK TRANSACTION`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/procedural-language#rollback_transaction) statement. This statement abandons all changes made inside the transaction.
- The query ends before reaching either of these two statements. In that case, BigQuery automatically rolls back the transaction.

If an error occurs during a transaction and the query has an [exception handler](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/procedural-language#beginexceptionend) , then BigQuery transfers control to the exception handler. Inside the exception block, you can choose whether to commit or roll back the transaction.

When session mode is disabled, if an error occurs during a transaction and there is no exception handler, then the query fails and BigQuery automatically rolls back the transaction.

When session mode is enabled and there is no exception handler, the transaction remains active until the session is terminated or it is explicitly rolled back. Regardless of the session mode, it is good practice to add exception handlers to ensure transactions are properly rolled back as soon as possible to avoid holding locks and resources longer than necessary.

The following example shows an exception handler that rolls back a transaction:

```
BEGIN

  BEGIN TRANSACTION;
  INSERT INTO mydataset.NewArrivals
    VALUES ('top load washer', 100, 'warehouse #1');
  -- Trigger an error.
  SELECT 1/0;
  COMMIT TRANSACTION;

EXCEPTION WHEN ERROR THEN
  -- Roll back the transaction inside the exception handler.
  SELECT @@error.message;
  ROLLBACK TRANSACTION;
END;
```

## Statements supported in transactions

The following statement types are supported in transactions:

- Query statements: `SELECT`

- DML statements: `INSERT` , `UPDATE` , `DELETE` , `MERGE` , and `TRUNCATE TABLE`

- DDL statements on temporary entities:

  - `CREATE TEMP TABLE`
  - `CREATE TEMP FUNCTION`
  - `DROP TABLE` on a temporary table
  - `DROP FUNCTION` on a temporary function

DDL statements that create or drop permanent entities, such as datasets, tables, and functions, are not supported inside transactions.

### Date/time functions in transactions

Within a transaction, the following date/time functions have special behaviors:

- The [`CURRENT_TIMESTAMP`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/timestamp_functions#current_timestamp) , [`CURRENT_DATE`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/date_functions#current_date) , and [`CURRENT_TIME`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/time_functions#current_time) functions return the timestamp when the transaction started.

- You cannot use the [`FOR SYSTEM_TIME AS OF`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/query-syntax#for_system_time_as_of) clause to read a table beyond the timestamp when the transaction started. Doing so returns an error.

## Example of a transaction

This example assumes there are two tables named `Inventory` and `NewArrivals` , created as follows:

```
CREATE OR REPLACE TABLE mydataset.Inventory
(
 product string,
 quantity int64,
 supply_constrained bool
);

CREATE OR REPLACE TABLE mydataset.NewArrivals
(
 product string,
 quantity int64,
 warehouse string
);

INSERT mydataset.Inventory (product, quantity)
VALUES('top load washer', 10),
     ('front load washer', 20),
     ('dryer', 30),
     ('refrigerator', 10),
     ('microwave', 20),
     ('dishwasher', 30);

INSERT mydataset.NewArrivals (product, quantity, warehouse)
VALUES('top load washer', 100, 'warehouse #1'),
     ('dryer', 200, 'warehouse #2'),
     ('oven', 300, 'warehouse #1');
```

The `Inventory` table contains information about current inventory, and `NewArrivals` contains information about newly arrived items.

The following transaction updates `Inventory` with new arrivals and deletes the corresponding records from `NewArrivals` . Assuming that all statements complete successfully, the changes in both tables are committed atomically as a single transaction.

```
BEGIN TRANSACTION;

-- Create a temporary table that holds new arrivals from 'warehouse #1'.
CREATE TEMP TABLE tmp
  AS SELECT * FROM mydataset.NewArrivals WHERE warehouse = 'warehouse #1';

-- Delete the matching records from the NewArravals table.
DELETE mydataset.NewArrivals WHERE warehouse = 'warehouse #1';

-- Merge the records from the temporary table into the Inventory table.
MERGE mydataset.Inventory AS I
USING tmp AS T
ON I.product = T.product
WHEN NOT MATCHED THEN
 INSERT(product, quantity, supply_constrained)
 VALUES(product, quantity, false)
WHEN MATCHED THEN
 UPDATE SET quantity = I.quantity + T.quantity;

-- Drop the temporary table and commit the transaction.
DROP TABLE tmp;

COMMIT TRANSACTION;
```

## Transaction concurrency

If a transaction updates or deletes rows in a table, then the transaction acquires a table-level lock rather than locking individual partitions. Other transactions or DML statements that mutate rows in the same table can't run concurrently, even if they target different partitions of a partitioned table. BigQuery cancels conflicting transactions. However, BigQuery queues conflicting DML statements that run outside of a transaction to run later, subject to [queuing limits](https://docs.cloud.google.com/bigquery/docs/data-manipulation-language#update_delete_merge_dml_concurrency) .

> **Warning:** BigQuery automatically queues up to 20 standalone mutating DML statements per table. However, multi-statement transactions have a concurrency limit of 1 mutating transaction per table, and BigQuery doesn't queue them. If a concurrent multi-statement transaction attempts to modify a locked table, then the transaction bypasses queuing and fails immediately with the following error message: `Transaction is aborted due to concurrent update against table` .

When you run a mutating DML statement inside a multi-statement transaction, BigQuery holds the write lock on the target table until the transaction commits or rolls back, or until the parent multi-statement query or session ends. This behavior applies to transactions in a [multi-statement query](https://docs.cloud.google.com/bigquery/docs/multi-statement-queries) , a [stored procedure](https://docs.cloud.google.com/bigquery/docs/procedures) , or an active [session](https://docs.cloud.google.com/bigquery/docs/sessions-intro) , such as an ODBC or JDBC connection. BigQuery doesn't release the lock when the individual DML statement finishes. Long-running computations in the same open transaction keep the table locked and cause concurrent mutating transactions on that table to fail.

Operations that read or append new rows can run concurrently with the transaction. For example, any of the following operations can be performed concurrently on a table while a transaction mutates data in the same table:

- `SELECT` statements
- BigQuery Storage Read API read operations
- Queries from BigQuery BI Engine
- `INSERT` statements
- Load jobs that use `WRITE_APPEND` disposition to append rows
- Streaming writes

If a transaction only reads a table or appends new rows to it, any operation can be performed concurrently on that table because `INSERT` statements don't conflict with each other. To prevent multiple transactions that would otherwise only insert rows from running concurrently on a table, start each transaction with a `DELETE` statement that modifies no rows, such as `DELETE FROM `` TABLE_NAME `` WHERE FALSE;` . Including this statement causes BigQuery to acquire a table-level lock on the `DELETE` statement, so any other concurrent transaction that also runs a mutating statement on that table conflicts and is canceled.

### Avoid and resolve concurrency conflicts

To reduce transaction concurrency conflicts, use the following practices:

- **Remove unnecessary transaction wrappers** : if a stored procedure or multi-statement query runs only a single mutating DML statement, such as a `MERGE` statement, then don't wrap the statement in `BEGIN TRANSACTION` and `COMMIT TRANSACTION` statements. A single DML statement is already atomic. Running the statement outside of a multi-statement transaction lets BigQuery apply [DML queuing](https://docs.cloud.google.com/bigquery/docs/data-manipulation-language#update_delete_merge_dml_concurrency) .
- **Retry failed transactions with exponential backoff** : when a multi-statement transaction fails because of a concurrent update, BigQuery rolls back the transaction and releases the table lock. Because BigQuery can take several seconds to release the lock in the metadata layer, retry failed transactions by using an exponential backoff strategy. Wait at least 60 seconds before the first retry.

## Viewing transaction information

BigQuery assigns a transaction ID to each multi-statement transaction. The transaction ID is attached to each query that executes inside the transaction. To view the transaction IDs for your jobs, query the [`INFORMATION_SCHEMA.JOBS*`](https://docs.cloud.google.com/bigquery/docs/information-schema-jobs) views for the `transaction_id` column.

When a multi-statement transaction runs, BigQuery creates a child job for each statement in the transaction. For a given transaction, every child job that is associated with that transaction has the same `transaction_id` value.

The following examples show how to find information about your transactions.

### Find all committed or rolled back transactions

The following query returns all transactions that were successfully committed.

```
SELECT transaction_id, parent_job_id, query
FROM `region-us`.INFORMATION_SCHEMA.JOBS_BY_PROJECT
WHERE statement_type = "COMMIT_TRANSACTION" AND error_result IS NULL;
```

The following query returns all transactions that were successfully rolled back.

```
SELECT
  transaction_id, parent_job_id, query
FROM `region-us`.INFORMATION_SCHEMA.JOBS_BY_PROJECT
WHERE statement_type = "ROLLBACK_TRANSACTION" AND error_result IS NULL;
```

### Find the start and end time of a transaction

The following query returns the starting and ending times for a specified transaction ID.

```
SELECT transaction_id, start_time, end_time, statement_type
FROM `region-us`.INFORMATION_SCHEMA.JOBS_BY_USER
WHERE transaction_id = "TRANSACTION_ID"
AND statement_type IN
  ("BEGIN_TRANSACTION", "COMMIT_TRANSACTION", "ROLLBACK_TRANSACTION")
ORDER BY start_time;
```

### Find the transaction in which a job is running

The following query gets the transaction associated with a specified job ID. It returns `NULL` if the job is not running within a multi-statement transaction.

```
SELECT transaction_id
FROM `region-us`.INFORMATION_SCHEMA.JOBS_BY_PROJECT
WHERE job_id = 'JOB_ID';
```

### Find the current job running within a transaction

The following query returns information about the job that is currently running within a specified transaction, if any.

```
SELECT job_id, query, start_time, total_slot_ms
FROM `region-us`.INFORMATION_SCHEMA.JOBS_BY_PROJECT
WHERE transaction_id = 'TRANSACTION_ID' AND state = RUNNING;
```

### Find the active transactions that affect a table

The following query returns the active transactions that affect a specified table. For each active transaction, if the transaction is running as part of multi-statement queries such as within a [stored procedure](https://docs.cloud.google.com/bigquery/docs/procedures) , then it also returns the parent job ID. If the transaction is running within a session, then it also returns the session info.

```
WITH running_transactions AS (
  SELECT DISTINCT transaction_id
  FROM
    `region-us`.INFORMATION_SCHEMA.JOBS_BY_PROJECT
  EXCEPT DISTINCT
  SELECT transaction_id
  FROM
    `region-us`.INFORMATION_SCHEMA.JOBS_BY_PROJECT
  WHERE
    statement_type = 'COMMIT_TRANSACTION'
    OR statement_type = 'ROLLBACK_TRANSACTION'
)
SELECT
  jobs.transaction_id, parent_job_id, session_info, query
FROM
  `region-us`.INFORMATION_SCHEMA.JOBS_BY_PROJECT AS jobs, running_transactions
WHERE
  destination_table = ("PROJECT_NAME", "DATASET_NAME", "TABLE_NAME")
  AND jobs.transaction_id = running_transactions.transaction_id;
```

### Find the active transactions running in a multi-statement transaction

The following query returns the active transactions for a particular job, specified by the ID of the job that is running the multi-statement transaction.

```
SELECT DISTINCT transaction_id
FROM
  `region-us`.INFORMATION_SCHEMA.JOBS_BY_PROJECT
WHERE
  parent_job_id = "JOB_ID"
EXCEPT DISTINCT
SELECT transaction_id
FROM
  `region-us`.INFORMATION_SCHEMA.JOBS_BY_PROJECT
WHERE
  parent_job_id = "JOB_ID"
  AND (statement_type = 'COMMIT_TRANSACTION'
       OR statement_type = 'ROLLBACK_TRANSACTION');
```

### Find overlapping jobs for a failed transaction

When a transaction or DML statement fails with a concurrent update error, a query history check of individual DML statements might not show an active DML statement at the time of the failure. This situation occurs when a parent multi-statement query or session runs an earlier DML statement inside a transaction and continues running other statements while holding the table lock.

For cross-project workloads, you can query the [`INFORMATION_SCHEMA.JOBS_BY_ORGANIZATION`](https://docs.cloud.google.com/bigquery/docs/information-schema-jobs-by-organization) view instead of `INFORMATION_SCHEMA.JOBS_BY_PROJECT` . The following query joins child DML jobs with their parent jobs in `INFORMATION_SCHEMA.JOBS_BY_PROJECT` to identify mutating jobs, scripts, or sessions that overlapped with a failed job:

```
SELECT
  child_jobs.job_id,
  child_jobs.parent_job_id,
  child_jobs.transaction_id,
  child_jobs.session_info.session_id,
  child_jobs.statement_type,
  child_jobs.start_time AS statement_start_time,
  child_jobs.end_time AS statement_end_time,
  parent_jobs.start_time AS parent_start_time,
  parent_jobs.end_time AS parent_end_time,
  COALESCE(parent_jobs.start_time, child_jobs.start_time) AS lock_window_start,
  COALESCE(
    parent_jobs.end_time, child_jobs.end_time, CURRENT_TIMESTAMP()
  ) AS lock_window_end,
  child_jobs.error_result.message AS error_message,
  child_jobs.query
FROM
  `region-REGION`.INFORMATION_SCHEMA.JOBS_BY_PROJECT AS child_jobs
LEFT JOIN
  `region-REGION`.INFORMATION_SCHEMA.JOBS_BY_PROJECT AS parent_jobs
  ON child_jobs.parent_job_id = parent_jobs.job_id
WHERE
  child_jobs.creation_time
    BETWEEN TIMESTAMP_SUB(TIMESTAMP 'START_TIME', INTERVAL 6 HOUR)
    AND TIMESTAMP_ADD(TIMESTAMP 'END_TIME', INTERVAL 1 HOUR)
  AND child_jobs.destination_table.project_id = 'PROJECT_ID'
  AND child_jobs.destination_table.dataset_id = 'DATASET_NAME'
  AND child_jobs.destination_table.table_id = 'TABLE_NAME'
  AND child_jobs.statement_type IN (
    'UPDATE', 'DELETE', 'MERGE', 'TRUNCATE_TABLE'
  )
  AND COALESCE(parent_jobs.start_time, child_jobs.start_time)
    <= TIMESTAMP 'END_TIME'
  AND COALESCE(parent_jobs.end_time, child_jobs.end_time, CURRENT_TIMESTAMP())
    >= TIMESTAMP 'START_TIME'
ORDER BY
  lock_window_start;
```

Replace the following:

- `REGION` : the dataset region (for example, `us` ).
- `START_TIME` : the `start_time` timestamp of the failed job (for example, `2026-06-15 10:00:00 UTC` ).
- `END_TIME` : the `end_time` timestamp of the failed job (for example, `2026-06-15 10:05:00 UTC` ).
- `PROJECT_ID` : the ID of the project that contains the target table.
- `DATASET_NAME` : the name of the dataset that contains the target table.
- `TABLE_NAME` : the name of the target table.

## Limitations

- Transactions cannot use DDL statements that affect permanent entities.

- Within a transaction, [materialized views](https://docs.cloud.google.com/bigquery/docs/materialized-views-intro) are interpreted as logical views. You can still query a materialized view inside a transaction, but it doesn't result in any performance improvement or cost reduction compared with the equivalent logical view.

- A multi-statement transaction that fails triggers a rollback operation, undoing all pending changes and precluding retries.

- A transaction can mutate data in at most 100 tables and can perform at most 100,000 partition modifications.

- [BI Engine](https://docs.cloud.google.com/bigquery/docs/bi-engine-intro) does not accelerate queries inside a transaction.

- Metadata for external data sources can't be refreshed within a transaction using [a system procedure](https://docs.cloud.google.com/bigquery/docs/reference/system-procedures#bqrefresh_external_metadata_cache) .
