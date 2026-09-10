# Development persistence

The emulator still runs entirely in memory. Opt in to native checkpoints with:

```sh
emulator_main --host_port=0.0.0.0:9010 \
  --state_file=/data/spanner.snapshot --checkpoint_interval=60s
```

The same flags work with `gateway_main`. To replace the image's default
command while keeping its HTTP gateway:

```sh
docker run --rm -p 9010:9010 -p 9020:9020 \
  -v spanner-dev-state:/data YOUR_EMULATOR_IMAGE \
  ./gateway_main --hostname=0.0.0.0 \
  --state_file=/data/spanner.snapshot --checkpoint_interval=60s
```

Mount a writable directory at `/data`. The directory must already exist. Each
emulator process needs its own state file. Omit `--state_file` for
an ephemeral emulator; `--checkpoint_interval` defaults to 60 seconds and must
be finite and positive when persistence is enabled.

Startup restores the file **before opening the gRPC listening port**. A missing
file starts empty. Invalid, truncated, incompatible or otherwise unrestorable
files cause startup to fail with an error; they are never treated as an empty
database. There is no automatic import of SQL dumps.

SIGINT and SIGTERM drain/cancel requests, stop background workers and checkpoint
committed state before exit. The gateway forwards both signals and waits for the
native process. Allow enough container shutdown time for the final checkpoint.
A failed final checkpoint is logged and produces a nonzero exit status. Periodic
checkpoint failures are logged and retried at the next interval.

## What is saved

* Instances and instance partitions, including their metadata.
* Databases and their current schemas, including proto descriptors.
* Committed base-table rows and change-stream records/partition tokens.
* Named sequence and identity counters, including allocations consumed by
  transactions that later rolled back. Untouched sequences remain untouched.

Sessions, active transactions, uncommitted rows, read snapshots, query caches,
admin operation handles and dropped schema history are not saved. Secondary
indexes, including foreign-key indexes, are rebuilt from base rows on restore.

Each database's schema and rows are captured consistently using the same
snapshot barrier and before-images as concurrent read-only transactions. The
single-writer/concurrent-strong-reader policy is unchanged. A checkpoint briefly
pins a schema and registers a storage snapshot; serialization and file I/O run
after releasing the commit barrier. Reading each table into the checkpoint uses
the storage mutex, as other scans do. The database is pinned while its snapshot
is being serialized so concurrent database deletion is safe.

Database snapshots are individually consistent. The file does **not** promise
one common transaction timestamp across different databases. Changes arriving
during checkpointing remain dirty for a later checkpoint. Clean checkpoints
inspect collection/storage revisions and sequence counters and skip file I/O.
Ordinary writes only increment an in-memory storage revision under the mutex
that they already hold; they never wait for a disk write.

## File format and limits

Format version 1 is a binary protobuf inside an envelope containing `SPANSNAP`,
a little-endian version, payload length and CRC32C. It uses GoogleSQL's value
encoding, with explicit normalized representations for PostgreSQL numeric and
JSONB values (including arrays), and distinguishes absent cells from typed NULL.
It stores current schema definitions as parsed DDL protobufs, rather than a
migration log. Restore builds those definitions into one private schema graph,
maintaining its catalog as objects are added. It checks new nodes as it goes,
then canonicalizes and validates the complete graph once before publication.
It reuses the existing schema validators and index backfill implementation and
loads base rows directly into storage without SQL INSERTs. Ordinary migrations
still use their existing schema generations and validation path.

Runtime IDs are regenerated. Rows identify a table by its name (or a change
stream by its name and internal-table role); columns map by name and type.
Identity counters map by owning table and column, named sequences by name.
Indexes and generated internal sequence names are not used as persistent IDs.
PostgreSQL catalog OIDs are regenerated.

A checkpoint is written to a uniquely named temporary file beside the
specified destination, flushed and closed, then published by atomic rename.
An interrupted temporary file is ignored on startup. The previous checkpoint
remains readable until replacement completes. Temporary files left by a crash
can be removed manually. A `.lock` sidecar prevents two processes from owning
the same path concurrently; the small sidecar is intentionally retained.

This is development persistence, not crash-proof durability. SIGKILL, a machine
crash or power loss can lose changes since the last successful checkpoint.
There is no WAL, history retention or incremental checkpoint system. Snapshot
files contain database contents in plaintext binary form and are created with
mode 0600. The payload is limited to 2 GiB. A checkpoint temporarily needs memory
for its protobuf and serialized payload, plus a materialized table scan and
before-images for rows concurrently changed. Restore temporarily holds the
snapshot alongside the restored database and rebuilt indexes. Use a matching
fork/format version; this is not a portable Cloud Spanner backup format.

## Subsequent Found integration (separate work)

`development/spanner/main.go` currently starts the native process, initializes
resources, replays SQL dumps, and periodically exports SQL. On shutdown it
exports again and kills the child. To use native persistence:

1. Pass `--state_file` and `--checkpoint_interval` to the **primary development**
   emulator and mount a persistent directory. Keep test emulators ephemeral.
2. Wait for native readiness before opening the proxy. Initialize the instance
   and development database only if they do not exist after restore.
3. Remove periodic and shutdown SQL exports, startup SQL replay, and any
   restart policy retained solely for the old export/replay approach.
4. Send SIGTERM to the primary child and wait for its exit instead of calling
   `Process.Kill()`. Budget time for its final checkpoint.
5. Handle initial migration from existing SQL dumps explicitly: load once into
   an empty emulator with persistence enabled, then shut it down gracefully.
6. Retain or replace the separate test-emulator schema/migration-metadata
   seeding workflow; a development state file also contains development data.

No Found files are changed by this implementation.

## Validation and benchmarks

The measurements below describe the original persistence implementation. The
schema construction optimization and its additional validation are described
under "Single-pass schema restore" at the end.

The implementation adds backend round-trip/concurrency tests and frontend
file/lifecycle tests. Benchmark targets:

```sh
bazel test -c opt //backend/database:snapshot_test \
  //frontend/server:persistence_test //tests/persistence:process_test
bazel run -c opt //frontend/server:persistence_benchmark -- \
  --benchmark_min_time=1s --benchmark_repetitions=5
```

Optimized Apple Silicon build, five repetitions, median wall times, local
filesystem. The row fixture has three columns, a 128-byte string per row, and one
secondary index. Checkpoint timing includes serialization, fsync and rename;
restore includes reading, validating, loading the schema/rows, index rebuilding
and disposal of the restored database.

| Rows | File size | Checkpoint | Restore |
| ---: | ---: | ---: | ---: |
| 1,000 | 157 KiB | 1.53 ms | 6.14 ms |
| 10,000 | 1.54 MiB | 11.46 ms | 57.52 ms |
| 100,000 | 15.59 MiB | 110.98 ms | 645.16 ms |

A clean checkpoint takes **0.47 microseconds** on the 10,000-row fixture and does
not touch the file. Restoring schemas with 128/512/1,024 empty tables takes
15.4/209.9/846.0 ms. Large current schemas still incur catalog validation and
schema graph rebuilding; historical migrations do not add to restore work.

With the default 60-second interval enabled, ordinary writes take 29.51 versus
29.54 microseconds with checkpointing disabled (-0.1%), and repeated queries take
8.970 versus 8.971 ms. Those short trials measure the idle worker's overhead;
they do not include a periodic checkpoint. Stressing checkpointing at **60 ms**
(one-thousandth of the default interval) while continuously updating the 10,000-row
fixture raises write time to 31.45 microseconds (+6.4%). The query fixture uses
the current emulator scan path; these figures are comparisons, not a promise
about indexed point-query latency.

Against the performance fork before persistence, the existing 1,024-step
migration benchmark changes from **836.09 to 836.65 ms (+0.07%)**. Repeated
streaming-query benchmarks at 128 tables change by -0.3% to +0.4%; a trivial
zero-table query changes from 222.5 to 230.1 microseconds (+3.4%). These are small
microbenchmark variations, with no significant regression in the tested query
or migration workloads. Results vary with hardware, filesystem and data shape.

Validation passed on macOS and native Linux. The new tests comprise 13 backend
round-trip/concurrency cases, seven frontend file/lifecycle cases, and two real
process cases. They cover typed values (including PostgreSQL values and
proto/enum descriptors), changed runtime IDs, schema changes, indexes and cyclic
foreign keys, change streams, identity/sequence continuation, buffered writes,
concurrent commits/readers, resource deletion, unchanged checkpoints, file-lock
ownership, failed saves/retry, corruption, truncated or orphan temporary files,
SIGTERM through the gateway, and recovery after SIGKILL.

The broader regression pass also covers database, storage, read-only and
read-write transactions, query/transaction handlers, schema updater/catalog,
sequences, and change-stream churner tests. Existing disabled schema-updater
cases remain disabled. The native and gateway binaries were built and exercised
on both platforms. This is not a full Cloud Spanner conformance-suite run.

On Coder's Linux x86-64 host (optimized build, three repetitions), the same
1,000/10,000/100,000-row fixture checkpoints in 3.11/14.48/141.51 ms and restores
in 4.53/35.69/400.94 ms. The clean check takes 0.29 microseconds. Query/write
comparisons use an idle thread in **both** cases to reflect an RPC server: otherwise Linux's
single-thread reference-count optimizations bias the disabled baseline.
With this correction and seven repetitions, default-interval writes take
15.51 versus 15.46 microseconds (+0.3%), and queries take 7.43 versus 7.33 ms
(+1.4%). Continuous writes with a 60 ms interval take 16.37 microseconds (+5.9%).

Found's real `api-backend/site/db/spanner_structure.sql` was also loaded into an
isolated emulator in Coder: **1,479 DDL definitions, 479 migration versions and
one internal metadata row**. A graceful checkpoint produced a 391 KiB file in
64 ms including process shutdown. Three restarts preserved the DDL, migration
metadata, 4,434 column entries and 1,480 index entries exactly. Clean shutdowns
did not rewrite the file.

Restore-to-gateway-readiness took **9.51 seconds median** (9.36–14.13 seconds)
compared with 10.89 seconds for the initial SQL schema/metadata load (one DDL
statement per request).
A further measured restart used **64.7 MiB peak native RSS at readiness** and
81.7 MiB including the verification queries. The SQL comparison measures the CI
loader, not Found's Go wrapper or an export of a populated development database.
This schema-heavy case is still dominated by schema graph validation/rebuilding;
the benefit of direct binary row loading grows with development data volume.
Found's wrapper and RSpec setup were not changed or exercised as part of this
persistence implementation.

## Single-pass schema restore

The restore path now constructs the current schema once instead of cloning and
validating its growing graph after every definition. It accepts the dependency
ordered current definitions emitted by the snapshot serializer, including
foreign keys emitted after their tables. Destructive migrations and object
replacements are rejected in snapshots. This changes neither format version 1
nor transaction concurrency, and existing snapshot files need no conversion.

On an optimized native Apple Silicon build, a real development snapshot with
two databases, 356 tables, 1,136 secondary index definitions and 614 rows restored
in **0.267 seconds**, compared with **12.77 seconds** before this change (about
48 times faster). These are the backend restore log timings, excluding process
and HTTP gateway startup. The 516 KiB file was restored, checkpointed and
restored again; the checkpoint was byte-for-byte identical and every table,
column, index definition and row fingerprint matched across restarts.

Four additional backend tests cover many interacting indexes, post-restore
migrations while an old reader retains its schema, dependent functions and
views, malformed current definitions, and continued PostgreSQL OID assignment.
The existing sequence, foreign-key, typed-value, corruption and concurrency
tests also exercise this construction path. The persistence benchmark includes
`BM_IndexSchemaRestore` at 128, 512 and 1,024 indexes to track schema-heavy
restores separately from row loading.
