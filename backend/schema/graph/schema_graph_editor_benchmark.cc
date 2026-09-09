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
#include <utility>

#include "benchmark/benchmark.h"
#include "backend/schema/graph/schema_graph.h"
#include "backend/schema/graph/schema_graph_editor.h"
#include "backend/schema/graph/schema_graph_editor_test_node.h"
#include "backend/schema/updater/schema_validation_context.h"

namespace google::spanner::emulator::backend::test {
namespace {

// Models a migration chain that adds a table-sized group of nodes per step.
// Uses the real graph editor, including cloning, fixups and validation passes;
// excludes SQL parsing, storage backfills, RPCs and catalog construction.
void BM_GrowingMigrationChain(benchmark::State& state) {
  constexpr int kNodesPerMigration = 8;
  for (auto _ : state) {
    auto graph = std::make_unique<SchemaGraph>();
    for (int migration = 0; migration < state.range(0); ++migration) {
      SchemaValidationContext context;
      SchemaGraphEditor editor(graph.get(), &context);
      const auto* dependency = graph->GetSchemaNodes().empty()
                                   ? nullptr
                                   : graph->GetSchemaNodes().front()
                                         ->As<GraphTestNode>();
      for (int i = 0; i < kNodesPerMigration; ++i) {
        auto node = std::make_unique<GraphTestNode>(i);
        if (dependency != nullptr) node->edges.push_back(dependency);
        auto status = editor.AddNode(std::move(node));
        if (!status.ok()) {
          state.SkipWithError(status.ToString());
          return;
        }
      }
      auto result = editor.CanonicalizeGraph();
      if (!result.ok()) {
        state.SkipWithError(result.status().ToString());
        return;
      }
      graph = std::move(*result);
    }
    benchmark::DoNotOptimize(graph.get());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_GrowingMigrationChain)->RangeMultiplier(2)->Range(128, 1024);

}  // namespace
}  // namespace google::spanner::emulator::backend::test

BENCHMARK_MAIN();
