// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>
#include <vector>

#include "benchmark/benchmark.h"
#include "absl/time/time.h"
#include "backend/storage/in_memory_storage.h"

namespace google::spanner::emulator::backend {
namespace {

// Includes allocation and destruction of the store. Distinct timestamps model
// committed updates within the original one-hour version retention window.
void BM_UpdateHotRow(benchmark::State& state) {
  const TableID table = "T";
  const Key key({googlesql::values::Int64(1)});
  const std::vector<ColumnID> columns = {"A", "B", "C", "D"};
  const std::vector<googlesql::Value> values(4, googlesql::values::Int64(42));
  for (auto _ : state) {
    InMemoryStorage storage;
    for (int i = 0; i < state.range(0); ++i) {
      auto status = storage.Write(absl::UnixEpoch() + absl::Microseconds(i),
                                  table, key, columns, values);
      if (!status.ok()) {
        state.SkipWithError(status.ToString());
        return;
      }
    }
    benchmark::DoNotOptimize(storage);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_UpdateHotRow)->Arg(1024)->Arg(8192);

// Isolates write cost while snapshots remain open. Setup and destruction are
// excluded. Later passes update the same rows without retaining more versions.
void BM_UpdateRowsWithSnapshots(benchmark::State& state) {
  const TableID table = "T";
  const std::vector<ColumnID> columns = {"A", "B", "C", "D"};
  const std::vector<googlesql::Value> initial(4, googlesql::values::Int64(0));
  const std::vector<googlesql::Value> updated(4, googlesql::values::Int64(42));
  std::vector<Key> keys;
  for (int i = 0; i < 8192; ++i) {
    keys.emplace_back(std::vector<googlesql::Value>{googlesql::values::Int64(i)});
  }
  for (auto _ : state) {
    state.PauseTiming();
    auto storage = std::make_unique<InMemoryStorage>();
    for (const auto& key : keys) {
      auto status = storage->Write(absl::UnixEpoch(), table, key, columns,
                                   initial);
      if (!status.ok()) {
        state.SkipWithError(status.ToString());
        return;
      }
    }
    std::vector<std::unique_ptr<Storage>> snapshots;
    for (int i = 0; i < state.range(0); ++i) {
      snapshots.push_back(storage->CreateSnapshot());
    }
    state.ResumeTiming();
    for (int pass = 0; pass < state.range(1); ++pass) {
      for (const auto& key : keys) {
        auto status = storage->Write(absl::UnixEpoch(), table, key, columns,
                                     updated);
        if (!status.ok()) {
          state.SkipWithError(status.ToString());
          return;
        }
      }
    }
    state.PauseTiming();
    snapshots.clear();
    storage.reset();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * keys.size() * state.range(1));
}
BENCHMARK(BM_UpdateRowsWithSnapshots)
    ->Args({0, 1})->Args({1, 1})->Args({4, 1})
    ->Args({0, 8})->Args({1, 8})->Args({4, 8});

// Measures scans after churn, excluding initial inserts/deletes. The old store
// traverses tombstoned keys; the current-value store contains only live keys.
void BM_ScanAfterDeletes(benchmark::State& state) {
  InMemoryStorage storage;
  const TableID table = "T";
  const std::vector<ColumnID> columns = {"A"};
  const std::vector<googlesql::Value> values = {googlesql::values::Int64(42)};
  const auto timestamp = absl::UnixEpoch();
  for (int i = 0; i < state.range(0); ++i) {
    auto status = storage.Write(timestamp, table,
                                Key({googlesql::values::Int64(i)}), columns,
                                values);
    if (!status.ok()) {
      state.SkipWithError(status.ToString());
      return;
    }
  }
  auto status = storage.Delete(
      timestamp + absl::Seconds(1), table,
      KeyRange::ClosedOpen(Key({googlesql::values::Int64(32)}), Key::Infinity()));
  if (!status.ok()) {
    state.SkipWithError(status.ToString());
    return;
  }
  for (auto _ : state) {
    std::unique_ptr<StorageIterator> rows;
    status = storage.Read(timestamp + absl::Seconds(2), table, KeyRange::All(),
                          columns, &rows);
    if (!status.ok()) {
      state.SkipWithError(status.ToString());
      return;
    }
    int count = 0;
    while (rows->Next()) ++count;
    benchmark::DoNotOptimize(count);
  }
}
BENCHMARK(BM_ScanAfterDeletes)->Arg(1024)->Arg(8192);

}  // namespace
}  // namespace google::spanner::emulator::backend

BENCHMARK_MAIN();
