// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0.
#include <filesystem>
#include <future>
#include <thread>

#include "absl/strings/str_cat.h"
#include "backend/persistence/files.h"
#include "benchmark/benchmark.h"
#include "frontend/server/persistence.h"
#include "googlesql/base/status_macros.h"

namespace google::spanner::emulator::frontend {
namespace {
using googlesql::values::Int64;
using googlesql::values::String;
constexpr char kDb[] = "projects/p/instances/i/databases/db";
// Real servers have RPC threads even with persistence disabled. In particular,
// glibc/libstdc++ optimize reference counts for single-threaded processes, so a
// threadless baseline would incorrectly charge that transition to persistence.
struct ThreadedRuntime {
  std::promise<void> done;
  std::future<void> stopped = done.get_future();
  std::thread idle{[this] { stopped.wait(); }};
  ~ThreadedRuntime() {
    done.set_value();
    idle.join();
  }
};
struct Fixture {
  ServerEnv env;
  std::string dir;
  std::shared_ptr<Database> db;
  std::unique_ptr<Persistence> state;
  ~Fixture() {
    state.reset();
    if (!dir.empty()) std::filesystem::remove_all(dir);
  }
  absl::Status Init(int rows) {
    dir = "/tmp/spanner-checkpoint-bench-XXXXXX";
    if (!mkdtemp(dir.data())) return absl::InternalError("mkdtemp");
    GOOGLESQL_ASSIGN_OR_RETURN(state, Persistence::Open(&env, dir + "/state"));
    GOOGLESQL_RETURN_IF_ERROR(env.instance_manager()
                                  ->CreateInstance("projects/p/instances/i", {})
                                  .status());
    GOOGLESQL_ASSIGN_OR_RETURN(
        db, env.database_manager()->CreateDatabase(
                kDb, {.statements = {"CREATE TABLE T (id INT64, v INT64, "
                                     "payload STRING(MAX)) PRIMARY KEY(id)",
                                     "CREATE INDEX ByV ON T(v)"}}));
    for (int first = 0; first < rows; first += 1000) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          auto writer, db->backend()->CreateReadWriteTransaction({}, {}));
      backend::Mutation mutation;
      std::vector<backend::ValueList> values;
      for (int i = first; i < std::min(first + 1000, rows); ++i)
        values.push_back({Int64(i), Int64(i), String(std::string(128, 'x'))});
      mutation.AddWriteOp(backend::MutationOpType::kInsert, "T",
                          {"id", "v", "payload"}, values);
      GOOGLESQL_RETURN_IF_ERROR(writer->Write(mutation));
      GOOGLESQL_RETURN_IF_ERROR(writer->Commit());
    }
    return absl::OkStatus();
  }
  absl::Status Write(int value) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        auto writer, db->backend()->CreateReadWriteTransaction({}, {}));
    backend::Mutation mutation;
    mutation.AddWriteOp(backend::MutationOpType::kUpdate, "T", {"id", "v"},
                        {{Int64(0), Int64(value)}});
    GOOGLESQL_RETURN_IF_ERROR(writer->Write(mutation));
    return writer->Commit();
  }
};
bool Check(benchmark::State& state, const absl::Status& status) {
  if (status.ok()) return true;
  state.SkipWithError(status.ToString());
  return false;
}
void BM_Checkpoint(benchmark::State& bench) {
  Fixture f;
  if (!Check(bench, f.Init(bench.range(0)))) return;
  int value = 0;
  for (auto _ : bench) {
    bench.PauseTiming();
    auto status = f.Write(++value);
    bench.ResumeTiming();
    if (!Check(bench, status) || !Check(bench, f.state->Checkpoint().status()))
      return;
  }
  bench.counters["file_bytes"] = std::filesystem::file_size(f.dir + "/state");
}
BENCHMARK(BM_Checkpoint)->Arg(1000)->Arg(10000)->Arg(100000)->UseRealTime();
void BM_Restore(benchmark::State& bench) {
  Fixture f;
  if (!Check(bench, f.Init(bench.range(0)))) return;
  if (!Check(bench, f.state->Checkpoint().status())) return;
  f.state.reset();
  for (auto _ : bench) {
    ServerEnv env;
    if (!Check(bench, Persistence::Open(&env, f.dir + "/state").status()))
      return;
  }
  bench.counters["file_bytes"] = std::filesystem::file_size(f.dir + "/state");
}
BENCHMARK(BM_Restore)->Arg(1000)->Arg(10000)->Arg(100000)->UseRealTime();
void BM_SchemaRestore(benchmark::State& bench) {
  Fixture f;
  if (!Check(bench, f.Init(0))) return;
  for (int i = 0; i < bench.range(0); ++i) {
    int count;
    absl::Time timestamp;
    absl::Status backfill;
    std::vector<std::string> statements = {absl::StrCat(
        "CREATE TABLE S", i, " (id INT64, v STRING(MAX)) PRIMARY KEY (id)")};
    if (!Check(bench,
               f.db->backend()->UpdateSchema({.statements = statements}, &count,
                                             &timestamp, &backfill)) ||
        !Check(bench, backfill))
      return;
  }
  if (!Check(bench, f.state->Checkpoint().status())) return;
  f.state.reset();
  for (auto _ : bench) {
    ServerEnv env;
    if (!Check(bench, Persistence::Open(&env, f.dir + "/state").status()))
      return;
  }
}
BENCHMARK(BM_SchemaRestore)->Arg(128)->Arg(512)->Arg(1024)->UseRealTime();
void BM_CleanCheckpoint(benchmark::State& bench) {
  Fixture f;
  if (!Check(bench, f.Init(10000)) ||
      !Check(bench, f.state->Checkpoint().status()))
    return;
  for (auto _ : bench)
    if (!Check(bench, f.state->Checkpoint().status())) return;
}
BENCHMARK(BM_CleanCheckpoint)->UseRealTime();
void BM_WriteWithPersistence(benchmark::State& bench) {
  ThreadedRuntime runtime;
  Fixture f;
  if (!Check(bench, f.Init(10000))) return;
  if (bench.range(0)) {
    f.state->Start(bench.range(0) == 1 ? absl::Seconds(60)
                                       : absl::Milliseconds(60));
  }
  int value = 0;
  for (auto _ : bench)
    if (!Check(bench, f.Write(++value))) return;
  f.state->Stop();
}
BENCHMARK(BM_WriteWithPersistence)->Arg(0)->Arg(1)->Arg(2)->UseRealTime();
void BM_QueryWithPersistence(benchmark::State& bench) {
  ThreadedRuntime runtime;
  Fixture f;
  if (!Check(bench, f.Init(10000))) return;
  if (bench.range(0)) {
    f.state->Start(bench.range(0) == 1 ? absl::Seconds(60)
                                       : absl::Milliseconds(60));
  }
  for (auto _ : bench) {
    auto reader = f.db->backend()->CreateReadOnlyTransaction({});
    if (!Check(bench, reader.status())) return;
    auto status = (*reader)->GuardedCall([&]() -> absl::Status {
      GOOGLESQL_ASSIGN_OR_RETURN(
          auto result,
          f.db->backend()->query_engine()->ExecuteSql(
              {.sql = "SELECT v, payload FROM T WHERE id = @id",
               .declared_params = {{"id", Int64(42)}}},
              {.schema = (*reader)->schema(), .reader = reader->get()}));
      while (result.rows->Next()) {
        auto value = result.rows->ColumnValue(0);
        benchmark::DoNotOptimize(value);
      }
      return result.rows->Status();
    });
    if (!Check(bench, status)) return;
  }
  f.state->Stop();
}
BENCHMARK(BM_QueryWithPersistence)->Arg(0)->Arg(1)->Arg(2)->UseRealTime();
}  // namespace
}  // namespace google::spanner::emulator::frontend
BENCHMARK_MAIN();
