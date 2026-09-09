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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_GRAPH_SCHEMA_GRAPH_EDITOR_TEST_NODE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_GRAPH_SCHEMA_GRAPH_EDITOR_TEST_NODE_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "backend/schema/graph/schema_graph_editor.h"
#include "backend/schema/graph/schema_node.h"

namespace google::spanner::emulator::backend::test {

// A small schema node for testing and benchmarking the real graph editor
// independently of SQL parsing. Edges can be shared or cyclic. Deleting a
// dependency cascades to its dependents.
class GraphTestNode : public SchemaNode {
 public:
  explicit GraphTestNode(int value) : value_(value) {}

  class Editor {
   public:
    explicit Editor(GraphTestNode* node) : node_(node) {}
    void set_value(int value) { node_->value_ = value; }
    void add_edge(const GraphTestNode* edge) { node_->edges.push_back(edge); }

   private:
    GraphTestNode* node_;
  };

  int value() const { return value_; }
  std::vector<const GraphTestNode*> edges;

  absl::Status Validate(SchemaValidationContext*) const override {
    return value_ < 0 ? absl::InvalidArgumentError("negative node value")
                      : absl::OkStatus();
  }

  absl::Status ValidateUpdate(const SchemaNode*,
                             SchemaValidationContext*) const override {
    return absl::OkStatus();
  }

  std::string DebugString() const override { return std::to_string(value_); }

 private:
  std::unique_ptr<SchemaNode> ShallowClone() const override {
    return std::make_unique<GraphTestNode>(*this);
  }

  absl::Status DeepClone(SchemaGraphEditor* editor,
                         const SchemaNode*) override {
    for (auto& edge : edges) {
      auto clone = editor->Clone(edge);
      if (!clone.ok()) return clone.status();
      edge = (*clone)->As<GraphTestNode>();
      if (edge->is_deleted()) MarkDeleted();
    }
    return absl::OkStatus();
  }

  int value_;
};

}  // namespace google::spanner::emulator::backend::test

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_GRAPH_SCHEMA_GRAPH_EDITOR_TEST_NODE_H_
