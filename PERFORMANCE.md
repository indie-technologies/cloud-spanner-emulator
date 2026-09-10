# Overall performance impact

The original measurements on this page compare the emulator at `fc811a1a`
with the performance fork before concurrent reads were restored,
including all schema graph, transaction, storage, schema lifetime, SQL
analysis, and query-catalog changes. These are cumulative comparisons, not just
the latest pass.

## Full migration and query workloads

Measured on macOS arm64 on September 9, 2026, using release builds and median
wall time over three repetitions. Both versions used the same benchmark source
and workload arguments. The original, pre-pass, and current binaries were run
sequentially. No compilation ran alongside the measurements.

| Workload | Original | Current | Total speedup |
| --- | ---: | ---: | ---: |
| 64 table migrations, one DDL batch | 11.44 ms | 4.83 ms | 2.37× |
| 64 table migrations, separate DDL calls | 22.48 ms | 4.81 ms | 4.67× |
| 256 table migrations, separate DDL calls | 419.17 ms | 53.15 ms | 7.89× |
| 1,024 table migrations, separate DDL calls | 12.23 s | 0.832 s | 14.70× |
| Streaming `SELECT 1`, default test schema | 2.057 ms | 0.225 ms | 9.13× |
| Streaming `SELECT 1`, 128 extra tables | 3.521 ms | 0.212 ms | 16.62× |
| Streaming empty table scan, 128 extra tables | 3.398 ms | 0.237 ms | 14.36× |

The complete migration benchmark process peaked at **1071.2 MiB**
resident memory originally and **40.1 MiB** now: **96.3% less**.
This is the process peak across the migration cases, including library and
allocator overhead; it is not a measurement of schema allocations alone.

Migrations create two-column tables and include database creation, DDL parsing,
validation, application, and database destruction. They do not include the first
write after DDL: the current implementation builds its final write-validation
registry on that first write. SQL measurements include the local gRPC round trip
and a single-use strong transaction, but exclude database/session/schema setup.
The table scan returns no rows.

## Component benchmarks

These earlier measurements also compare against the original implementation.
They isolate costs that may dominate particular workloads; their speedups must
not be multiplied by the full-workload results above.

| Component workload | Original | Current | Speedup |
| --- | ---: | ---: | ---: |
| Graph editing, 1,024 steps with eight nodes added per step | 19.0 s | 0.518 s | 36.7× |
| 1,024 updates to one four-column row | 593 µs | 171 µs | 3.5× |
| 8,192 updates to one four-column row | 6.87 ms | 1.38 ms | 5.0× |
| Scan after deleting 8,160 of 8,192 rows | 269 µs | 11.7 µs | 22.9× |

The graph benchmark excludes DDL parsing and real schema validation/backfills.
Storage benchmarks exclude the query and RPC layers. These component figures
are median CPU times, whereas the full-workload table reports wall times.
See the [graph benchmark](backend/schema/graph/README.md) and
[storage benchmark](backend/storage/README.md) for their methods.

## What produces the gains

- Graph cloning uses hash lookups instead of repeatedly scanning all schema nodes.
- Cells hold current values directly, and deletes physically remove rows.
- Committed schema history is reclaimed when no transaction or admin request
  references it. Migration count no longer controls retained schema history.
- Consecutive migrations avoid rebuilding write validators and their query
  catalogs. Only the first write against the final schema builds that registry.
- Streaming queries reuse SQL analysis for change-stream detection. DML and
  partitioned-DML validation also avoid redundant analysis passes.
- Commits skip unused change-stream bookkeeping.
- Schema lookup construction skips columns and key columns before trying the
  remaining object types. Tables without generated columns skip dependency-graph
  construction and cycle detection.
- SQL catalogs create table wrappers on demand, including named tables and
  synonyms, and collect column types only when requested. Analyzer options are
  owned once per request catalog and borrowed by table/column setup; only
  generated-column analysis copies them to add expression-column bindings.

These measurements used strong reads and one active transaction per database.
The fork now allows concurrent strong readers with one writer, retaining row
before-images only while snapshots need them. The measurements below predate
that restoration and are serial workloads, not concurrent-client throughput
measurements. Competing writers can still require retries.

There is no single measured whole-test-suite speedup: it depends on how much time
your tests spend in migrations, SQL analysis, storage, and work outside the
emulator. Growing migration chains still copy and validate progressively larger
schemas, so they do not become linear-time. Large DDL batches still retain
temporary schema graphs for validation and backfill failure handling.

## Additional gain from the latest optimization pass

This comparison isolates lazy SQL catalog construction, removal of redundant
analyzer-option copies, early exclusion of column/key nodes from schema lookup
construction, and skipping cycle detection for tables without generated columns.
The before version already includes the transaction, storage, and earlier
migration/query improvements.

| Workload | Before this pass | Current | Additional speedup |
| --- | ---: | ---: | ---: |
| 64 table migrations, one DDL batch | 8.12 ms | 4.83 ms | 1.68× |
| 64 table migrations, separate DDL calls | 8.36 ms | 4.81 ms | 1.74× |
| 256 table migrations, separate DDL calls | 106.33 ms | 53.15 ms | 2.00× |
| 1,024 table migrations, separate DDL calls | 1.808 s | 0.832 s | 2.17× |
| Streaming `SELECT 1`, default test schema | 0.222 ms | 0.225 ms | Approximately unchanged |
| Streaming `SELECT 1`, 128 extra tables | 0.980 ms | 0.212 ms | 4.62× |
| Streaming empty table scan, 128 extra tables | 0.993 ms | 0.237 ms | 4.20× |

Migration-process peak RSS is approximately unchanged by this pass: 39.8 MiB
before and 40.1 MiB after. Small-schema query times vary by about 12–13% across
these three repetitions, so the 0.004 ms difference is not evidence of a
regression. The large-schema query cases now take roughly the same time as
small-schema queries instead of paying to construct wrappers for unrelated
tables on every request.

The preceding profile used macOS `sample` for 10 seconds per workload with a
requested 1 ms interval. Before this pass, runtime type checks consumed about
46% of migration samples; SQL catalog construction consumed about 88% of
query-engine samples with 128 extra tables, including 63% in analyzer-option
copies. These are historical shares that motivated the changes, not profiles
of the current code. The table above measures the combined implementation;
it does not assign separate speedups to each optimization.

Each SQL request still owns its catalog, reader bindings, and security context.
Lazy wrappers retain stable identities for repeated lookups and enumeration.
The catalog owns analyzer options because the caller may pass a temporary;
generated-column analysis uses a separate copy so its expression-column bindings
do not affect another table. Tests cover cross-schema synonyms, enumeration
before lookup, and expressions analyzed after temporary options have expired.

## Remaining costs

Growing migration chains still clone and validate progressively larger schemas.
The [schema updater](backend/schema/updater/schema_updater.cc) also constructs
both temporary and final lookup catalogs. Avoiding those passes requires
preserving before/after schema views used by validation, backfills, and partial
DDL failure handling. Historical reads are no longer needed, but these temporary
views still are.

For repeated SQL on small schemas, analysis and plan preparation remain
significant. Caching analyzed statements/prepared plans needs schema invalidation,
correct option/parameter-type keys, and safe lifetimes for catalog references.
RPC transport and scheduling also remain part of the measured latency. Catalogs
still construct other object types, such as UDFs and property graphs, eagerly.

Populated tables have additional opportunities that these empty-scan benchmarks
do not quantify. `QueryableTable::CreateEvaluatorTableIterator` still requests
`KeySet::All()`, and [storage reads](backend/storage/in_memory_storage.cc)
materialize the selected range before returning an iterator. Passing primary-key
filters down to storage could turn a full scan into a key/range lookup. Reading
rows lazily could reduce copying and make limits cheaper, but must preserve
iterator stability during DML and transaction/request lifetimes. Those larger
changes are outside this pass.

## Reproduction and validation

Build the benchmark, then run migrations and RPC queries separately:

```sh
bazel build -c opt //frontend/handlers:performance_benchmark
bazel-bin/frontend/handlers/performance_benchmark \
  --benchmark_filter=BM_Migrations --benchmark_min_time=0.1s \
  --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
bazel-bin/frontend/handlers/performance_benchmark \
  --benchmark_filter=BM_StreamingQuery --benchmark_min_time=500x \
  --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

The query cases use 500 RPCs per repetition. Compare the **Time** column; the
**CPU** column excludes server threads. Process memory was measured with
child-process `getrusage` after the migration benchmark.

The original binary was built from the original tracked runtime sources and
build rules plus this same benchmark target. Current sources were backed up and
restored byte-for-byte after the baseline build. Both builds used identical
release flags and the temporary macOS PostgreSQL compatibility workarounds
listed in [the storage notes](backend/storage/README.md); those workarounds were
restored afterward.

The implementation passed **20 regression test targets**: the
[previous 16 targets](frontend/handlers/PERFORMANCE.md#validation), plus
`//backend/query:catalog_test`, `//backend/query:queryable_table_test`,
`//backend/query:query_validator_test`, and `//backend/schema/catalog:schema_test`.
The [latest-pass measurements](frontend/handlers/PERFORMANCE.md) use an
intermediate baseline that already includes the graph and storage changes; they
are retained for attribution and are not the total comparison on this page.
