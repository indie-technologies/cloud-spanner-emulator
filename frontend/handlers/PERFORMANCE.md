# Migration and SQL performance

This pass builds on the single-transaction, strong-read-only behavior and the
schema graph and single-version storage improvements already in this fork.

- The committed schema catalog keeps the current schema. Older schemas survive
  only while a transaction or admin request holds a shared snapshot. Schema
  replacement therefore does not accumulate an hour of migration history.
- Action registries and their query catalogs are built on the first write after
  a schema change. Consecutive migrations and transactions that only read avoid
  this work. The first write pays for one registry for the final schema.
- Streaming SQL uses the same analysis for change-stream detection and query
  execution. It no longer builds a temporary function catalog and analyzes
  ordinary SQL a second time.
- Recognized DML is analyzed with all target columns from the start. Partitioned
  DML validation also shares the execution analysis. The fallback for dialect
  statements the lightweight classifier does not recognize remains.
- Commits skip copying mutations and constructing change-stream tracking maps
  when the database has no change streams, or when there are no buffered writes.

## Measurements

Median wall time over three repetitions on macOS arm64, September 9, 2026,
release build. The baseline includes the earlier schema graph and single-version
storage changes; these numbers measure the additional improvements in this pass.
Both versions ran the same benchmark workloads and arguments.

| Workload | Before | After | Speedup |
| --- | ---: | ---: | ---: |
| 64 table migrations, one DDL batch | 7.76 ms | 7.41 ms | 1.05× |
| 64 table migrations, separate DDL calls | 17.94 ms | 7.68 ms | 2.33× |
| 256 table migrations, separate DDL calls | 265.06 ms | 99.43 ms | 2.67× |
| Streaming `SELECT 1`, default test schema | 1.328 ms | 0.189 ms | 7.04× |
| Streaming `SELECT 1`, 128 extra tables | 2.569 ms | 0.853 ms | 3.01× |
| Streaming empty table scan, 128 extra tables | 2.635 ms | 0.857 ms | 3.07× |

The migration workload creates two-column tables and includes database creation,
DDL validation/application, and database destruction. It does not include a first
write after migration. The SQL workload includes a local gRPC round trip and a
single-use strong transaction; database/session/schema setup is excluded. Use
the benchmark's **Time** column for RPC comparisons: its **CPU** column measures
the client thread and excludes server threads.

A separate one-iteration run of the 256-migration workload reduced peak process
resident memory from 98.2 MiB to 34.4 MiB, measured with child-process `getrusage`.
This includes process/library overhead, not just schema memory.

Run:

```sh
bazel run -c opt //frontend/handlers:performance_benchmark -- \
  --benchmark_min_time=0.1s --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=performance.json --benchmark_out_format=json
```

## Remaining costs

Batched migrations improve little because the updater still creates temporary
schema graphs for each statement. These support validating the whole batch
before backfills and publishing the successful prefix if a backfill fails.
Removing those copies safely requires changing the updater's staging algorithm.
Growing schemas still incur copying, lookup rebuilding, and validation work;
this change does not make arbitrary migration chains linear-time.

SQL still constructs a schema-dependent query catalog, analyzes and rewrites the
statement, prepares an evaluator, and serializes results. This pass does not
cache prepared queries or query results.

## Validation

Regression coverage includes schema reclamation across 1,024 replacements,
retained admin/aborted-transaction snapshots, first-write validators after DDL,
shared change-stream analysis in both dialects, and partitioned-DML validation
before any writes. Existing schema/backfill, SQL, transaction, admin, session,
partition, and change-stream suites are also exercised.

These 16 Bazel test targets passed in release mode:

```sh
bazel test -c opt \
  //backend/locking:manager_test \
  //backend/schema/catalog:versioned_catalog_test \
  //backend/database:database_test \
  //backend/database/change_stream:change_stream_partition_churner_test \
  //backend/query:query_engine_test \
  //backend/transaction:read_only_transaction_test \
  //backend/transaction:read_write_transaction_test \
  //backend/schema/updater:schema_updater_test \
  //backend/schema/backfills:column_value_backfill_test \
  //frontend/collections:multiplexed_session_transaction_manager_test \
  //frontend/handlers:databases_test \
  //frontend/handlers:instance_partitions_test \
  //frontend/handlers:queries_test \
  //frontend/handlers:transactions_test \
  //frontend/handlers:partitions_test \
  //frontend/handlers:change_streams_test
```

Local macOS testing uses the same temporary PostgreSQL platform workarounds
described in [the storage notes](../../backend/storage/README.md). Those build
workarounds are restored after each test run and are not shipped.
