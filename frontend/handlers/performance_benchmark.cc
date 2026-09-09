// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy at https://www.apache.org/licenses/LICENSE-2.0

#include <string>
#include <vector>

#include "benchmark/benchmark.h"
#include "google/spanner/v1/spanner.pb.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "backend/schema/updater/schema_updater.h"
#include "common/clock.h"
#include "tests/common/test_env.h"
#include "googlesql/base/status_macros.h"

namespace google::spanner::emulator {
namespace {

std::vector<std::string> Tables(int count) {
  std::vector<std::string> ddl;
  for (int i = 0; i < count; ++i) {
    ddl.push_back(absl::StrCat("CREATE TABLE T", i,
        " (Id INT64 NOT NULL, V STRING(20)) PRIMARY KEY (Id)"));
  }
  return ddl;
}

void BM_Migrations(benchmark::State& state) {
  const auto statements = Tables(state.range(0));
  for (auto _ : state) {
    Clock clock;
    auto database = backend::Database::Create(
        &clock, "benchmark", {.database_dialect =
            backend::database_api::DatabaseDialect::GOOGLE_STANDARD_SQL});
    if (!database.ok()) {
      state.SkipWithError(database.status().ToString());
      return;
    }
    auto apply = [&](const std::vector<std::string>& ddl) -> absl::Status {
      int successful;
      absl::Time timestamp;
      absl::Status backfill;
      auto status = (*database)->UpdateSchema(
          {.statements = ddl, .database_dialect =
              backend::database_api::DatabaseDialect::GOOGLE_STANDARD_SQL},
          &successful, &timestamp, &backfill);
      return status.ok() ? backfill : status;
    };
    absl::Status status;
    if (state.range(1)) {
      for (const auto& statement : statements) {
        status = apply({statement});
        if (!status.ok()) break;
      }
    } else {
      status = apply(statements);
    }
    if (!status.ok()) {
      state.SkipWithError(status.ToString());
      return;
    }
  }
}
BENCHMARK(BM_Migrations)
    ->Args({64, 0})
    ->Args({64, 1})
    ->Args({256, 1})
    ->Args({1024, 1});

class QueryServer : public test::ServerTest {
 public:
  void TestBody() override {}
  absl::Status Init(int tables) {
    GOOGLESQL_RETURN_IF_ERROR(CreateTestInstance());
    GOOGLESQL_RETURN_IF_ERROR(CreateTestDatabase());
    if (tables > 0) {
      GOOGLESQL_RETURN_IF_ERROR(UpdateDatabaseDdl(test_database_uri_, Tables(tables)));
    }
    GOOGLESQL_ASSIGN_OR_RETURN(session_, CreateTestSession(/*multiplexed=*/false));
    return absl::OkStatus();
  }
  absl::Status Run(const std::string& sql) {
    google::spanner::v1::ExecuteSqlRequest request;
    request.set_session(session_);
    request.set_sql(sql);
    std::vector<google::spanner::v1::PartialResultSet> results;
    return ExecuteStreamingSql(request, &results);
  }
 private:
  std::string session_;
};

void BM_StreamingQuery(benchmark::State& state) {
  QueryServer server;
  auto status = server.Init(state.range(0));
  if (!status.ok()) {
    state.SkipWithError(status.ToString());
    return;
  }
  const std::string sql = state.range(1)
      ? "SELECT int64_col FROM test_table" : "SELECT 1";
  for (auto _ : state) {
    status = server.Run(sql);
    if (!status.ok()) {
      state.SkipWithError(status.ToString());
      return;
    }
  }
}
BENCHMARK(BM_StreamingQuery)->Args({0, 0})->Args({128, 0})->Args({128, 1});

}  // namespace
}  // namespace google::spanner::emulator

BENCHMARK_MAIN();
