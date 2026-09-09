# Schema migration performance

Each DDL statement produces a new schema snapshot. The graph editor clones nodes,
rewrites their references, and validates the resulting graph. It preserves node
order, earlier snapshots, and per-statement validation even when many statements
are submitted in one request.

Original-node membership uses the graph's existing object-pool hash set. Cloning
checks the visited map before doing further work. This avoids scanning the whole
original graph on each node visit: membership work is expected linear in the
number of nodes and edges per cloning pass, instead of quadratic for sparse
graphs. A chain that grows the schema still visits larger graphs on later steps;
this change does not make total migration time linear or remove schema snapshots.

Run the regression tests with:

```sh
bazel test -c opt //backend/schema/graph:schema_graph_editor_test
bazel test -c opt //backend/schema/updater:schema_updater_test
```

Run the growing-chain benchmark with:

```sh
bazel run -c opt //backend/schema/graph:schema_graph_editor_benchmark -- \
  --benchmark_min_time=0.2s --benchmark_repetitions=3 \
  --benchmark_out=/tmp/schema-graph-benchmark.json --benchmark_out_format=json
```

Each iteration applies an entire chain of 128, 256, 512, or 1,024 graph changes,
adding eight nodes per change. It exercises the actual graph editor, including
cloning, reference fixups, and validation passes. The test nodes have lightweight
validators; these timings exclude SQL parsing, real schema validation rules,
function catalogs, storage backfills, and RPCs. Compare optimized builds on the
same machine, with no other build running, and measure application migrations
separately before interpreting the results as test-suite speedups.

Measured on macOS arm64 with `-c opt` (median CPU time, three repetitions):

| Migration steps | Before | After | Speedup |
| --- | ---: | ---: | ---: |
| 128 | 49.2 ms | 8.3 ms | 6.0x |
| 256 | 339.7 ms | 32.6 ms | 10.4x |
| 512 | 2,516.6 ms | 129.1 ms | 19.5x |
| 1,024 | 18,997.4 ms | 517.6 ms | 36.7x |

The baseline used the graph editor from commit `fc811a1a` with the same benchmark
source. Both builds used `--features=-layering_check` to work around a pre-existing
Clang dependency-check failure in gRPC. This flag is a build workaround, not part
of the runtime optimization.
