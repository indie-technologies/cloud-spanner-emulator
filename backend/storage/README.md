# Current-value storage with active snapshots

This fork stores the current value of each cell directly and physically removes
rows on deletion. It no longer allocates timestamp maps, maintains a versioned
row-existence column, collects expired cell versions, or traverses deleted rows
during scans when no snapshot needs those rows. Concurrent strong readers pin a
snapshot; the first subsequent change to each row saves its original value for
that snapshot. Closing the reader releases its before-images. No data is copied
at snapshot creation and no time-based row history is retained. Historical/stale
read options are rejected.

Writers still buffer changes until commit. A short publication lock excludes
snapshot registration during a commit or schema update, while existing readers
can continue reading their original data. Active SQL requests hold a shared
schema lock to protect functions that reference the latest schema during DDL.
Idle readers keep their schema and data without blocking later migrations.

The timestamp arguments on `Storage` remain for internal callers, including
schema backfills. They no longer select historical row values. The schema catalog
now retains only the current schema and snapshots still held by transactions or
admin requests. Temporary migration graphs and delayed cleanup of dropped schema
objects remain in place.

## Benchmark

Run:

```sh
bazel run -c opt //backend/storage:in_memory_storage_benchmark -- \
  --benchmark_min_time=0.2s --benchmark_repetitions=3
```

Measured on macOS arm64 on September 9, 2026, in a release build. These are median
CPU times over three repetitions, comparing the same benchmark against the
original storage implementation at `fc811a1a` and the implementation before
concurrent snapshots were restored.
The baseline was built under a separate class name; temporary baseline sources
and build targets were removed afterward.

| Workload | Original | Current | Speedup |
| --- | ---: | ---: | ---: |
| 1,024 updates to one four-column row | 593 µs | 171 µs | 3.5× |
| 8,192 updates to one four-column row | 6,873 µs | 1,375 µs | 5.0× |
| Scan after deleting 992 of 1,024 rows | 45.6 µs | 11.8 µs | 3.9× |
| Scan after deleting 8,160 of 8,192 rows | 269 µs | 11.7 µs | 22.9× |

The update benchmark includes store creation and destruction. Distinct write
timestamps fall within the original one-hour retention window. The scan benchmark
excludes setup and returns 32 live rows in both implementations.

These are storage microbenchmarks, not end-to-end SQL or migration timings.
Schema graph copying, SQL analysis, RPC handling, and transaction setup are not
measured here. See the [schema benchmark notes](../schema/graph/README.md) for
migration costs.

## Validation

The following test targets passed in release mode:

```sh
bazel test -c opt \
  //backend/storage:in_memory_storage_test \
  //backend/locking:manager_test \
  //backend/transaction:read_only_transaction_test \
  //backend/transaction:read_write_transaction_test \
  //backend/schema/updater:schema_updater_test \
  //frontend/converters:reads_test \
  //frontend/handlers:transactions_test \
  //frontend/handlers:queries_test \
  //frontend/handlers:partitions_test \
  //frontend/handlers:change_streams_test
```

The local macOS run required temporary build workarounds for the bundled
PostgreSQL Linux configuration (fortified string declarations, epoll, peer
credentials, `strerror_r`, file allocation/synchronization, and `librt`), plus
`--features=-layering_check`. Those workarounds were restored after testing;
no PostgreSQL platform changes are part of this patch.

Concurrency tests cover readers overlapping writers, atomic snapshot publication,
updates/deletes/reinserts, dropped schema objects, repeated reads across commits,
SQL that performs no storage reads, and multiplexed sessions. Randomized scans
are compared with independently copied maps. Database tests also check data and
index consistency across DDL and multi-row commits.
Change-stream tests retry complete operations on contention, including the
schema API's `FAILED_PRECONDITION` response. DML replay and rollback tests remain
part of the passing SQL/transaction suites.
