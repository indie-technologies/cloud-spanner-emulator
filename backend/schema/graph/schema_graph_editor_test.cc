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

#include "backend/schema/graph/schema_graph_editor.h"

#include <memory>
#include <utility>

#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "backend/schema/graph/schema_graph.h"
#include "backend/schema/graph/schema_graph_editor_test_node.h"
#include "backend/schema/updater/schema_validation_context.h"

namespace google::spanner::emulator::backend::test {
namespace {

TEST(SchemaGraphEditorTest, ClonesCyclesAndSharedEdgesOnce) {
  SchemaGraph original;
  auto first = std::make_unique<GraphTestNode>(1);
  auto second = std::make_unique<GraphTestNode>(2);
  auto* first_ptr = first.get();
  auto* second_ptr = second.get();
  first->edges = {second_ptr, second_ptr};
  second->edges = {first_ptr};
  original.Add(std::move(first));
  original.Add(std::move(second));

  SchemaValidationContext context;
  SchemaGraphEditor editor(&original, &context);
  auto result = editor.CanonicalizeGraph();
  ASSERT_TRUE(result.ok()) << result.status();
  auto nodes = (*result)->GetSchemaNodes();
  ASSERT_EQ(nodes.size(), 2);
  auto* cloned_first = nodes[0]->As<GraphTestNode>();
  auto* cloned_second = nodes[1]->As<GraphTestNode>();
  EXPECT_NE(cloned_first, first_ptr);
  EXPECT_NE(cloned_second, second_ptr);
  EXPECT_TRUE(original.Contains(first_ptr));
  EXPECT_FALSE(original.Contains(cloned_first));
  EXPECT_TRUE((*result)->Contains(cloned_first));
  EXPECT_FALSE((*result)->Contains(first_ptr));
  ASSERT_EQ(cloned_first->edges.size(), 2);
  EXPECT_EQ(cloned_first->edges[0], cloned_second);
  EXPECT_EQ(cloned_first->edges[1], cloned_second);
  ASSERT_EQ(cloned_second->edges.size(), 1);
  EXPECT_EQ(cloned_second->edges[0], cloned_first);
  EXPECT_EQ(first_ptr->edges[0], second_ptr);
  EXPECT_EQ(second_ptr->edges[0], first_ptr);
}

TEST(SchemaGraphEditorTest, EditsAndAddsWithoutChangingPreviousSnapshot) {
  SchemaGraph original;
  auto node = std::make_unique<GraphTestNode>(1);
  auto* original_node = node.get();
  original.Add(std::move(node));

  SchemaValidationContext context;
  SchemaGraphEditor editor(&original, &context);
  auto added = std::make_unique<GraphTestNode>(2);
  auto* added_ptr = added.get();
  added->edges = {original_node};
  ASSERT_TRUE(editor.AddNode(std::move(added)).ok());
  ASSERT_TRUE(editor.EditNode<GraphTestNode>(
      original_node, [added_ptr](GraphTestNode::Editor* edit) {
        edit->set_value(3);
        edit->add_edge(added_ptr);
        return absl::OkStatus();
      }).ok());
  ASSERT_TRUE(editor.EditNode<GraphTestNode>(
      added_ptr, [](GraphTestNode::Editor* edit) {
        edit->set_value(4);
        return absl::OkStatus();
      }).ok());

  auto result = editor.CanonicalizeGraph();
  ASSERT_TRUE(result.ok()) << result.status();
  auto nodes = (*result)->GetSchemaNodes();
  ASSERT_EQ(nodes.size(), 2);
  auto* updated = nodes[0]->As<GraphTestNode>();
  auto* new_node = nodes[1]->As<GraphTestNode>();
  EXPECT_EQ(updated->value(), 3);
  EXPECT_EQ(new_node->value(), 4);
  ASSERT_EQ(updated->edges.size(), 1);
  ASSERT_EQ(new_node->edges.size(), 1);
  EXPECT_EQ(updated->edges[0], new_node);
  EXPECT_EQ(new_node->edges[0], updated);
  EXPECT_EQ(original_node->value(), 1);
  EXPECT_TRUE(original_node->edges.empty());
}

TEST(SchemaGraphEditorTest, CascadingDeletionPreservesOtherNodesAndOldSnapshot) {
  SchemaGraph original;
  auto parent = std::make_unique<GraphTestNode>(1);
  auto child = std::make_unique<GraphTestNode>(2);
  auto* parent_ptr = parent.get();
  auto* child_ptr = child.get();
  child->edges = {parent_ptr};
  original.Add(std::move(child));
  original.Add(std::move(parent));
  original.Add(std::make_unique<GraphTestNode>(3));

  SchemaValidationContext context;
  SchemaGraphEditor editor(&original, &context);
  ASSERT_TRUE(editor.DeleteNode(parent_ptr).ok());
  auto result = editor.CanonicalizeGraph();
  ASSERT_TRUE(result.ok()) << result.status();
  auto nodes = (*result)->GetSchemaNodes();
  ASSERT_EQ(nodes.size(), 1);
  EXPECT_EQ(nodes[0]->As<GraphTestNode>()->value(), 3);
  EXPECT_TRUE((*result)->Contains(nodes[0]));
  EXPECT_FALSE((*result)->Contains(parent_ptr));
  EXPECT_FALSE((*result)->Contains(child_ptr));
  EXPECT_FALSE(parent_ptr->is_deleted());
  EXPECT_FALSE(child_ptr->is_deleted());
  EXPECT_EQ(original.GetSchemaNodes().size(), 3);
}

TEST(SchemaGraphEditorTest, EmptyGraphDoesNotContainExternalNodes) {
  SchemaGraph graph;
  GraphTestNode external(1);
  EXPECT_FALSE(graph.Contains(&external));
  EXPECT_FALSE(graph.Contains(nullptr));
  SchemaValidationContext context;
  SchemaGraphEditor editor(&graph, &context);
  EXPECT_FALSE(editor.DeleteNode(&external).ok());
  auto result = editor.CanonicalizeGraph();
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE((*result)->GetSchemaNodes().empty());
}

TEST(SchemaGraphEditorTest, FailedValidationPreservesOriginal) {
  SchemaGraph original;
  original.Add(std::make_unique<GraphTestNode>(1));
  SchemaValidationContext context;
  SchemaGraphEditor editor(&original, &context);
  ASSERT_TRUE(editor.EditNode<GraphTestNode>(
      original.GetSchemaNodes()[0], [](GraphTestNode::Editor* edit) {
        edit->set_value(-1);
        return absl::OkStatus();
      }).ok());
  auto result = editor.CanonicalizeGraph();
  EXPECT_TRUE(absl::IsInvalidArgument(result.status()));
  EXPECT_EQ(original.GetSchemaNodes()[0]->As<GraphTestNode>()->value(), 1);
}

TEST(SchemaGraphEditorTest, LongChainPreservesOrderAndRewritesDependencies) {
  auto graph = std::make_unique<SchemaGraph>();
  for (int migration = 0; migration < 128; ++migration) {
    SchemaValidationContext context;
    SchemaGraphEditor editor(graph.get(), &context);
    auto node = std::make_unique<GraphTestNode>(migration);
    if (migration > 0) {
      node->edges.push_back(
          graph->GetSchemaNodes().back()->As<GraphTestNode>());
    }
    ASSERT_TRUE(editor.AddNode(std::move(node)).ok());
    auto result = editor.CanonicalizeGraph();
    ASSERT_TRUE(result.ok()) << result.status();
    auto nodes = (*result)->GetSchemaNodes();
    ASSERT_EQ(nodes.size(), migration + 1);
    for (int i = 0; i <= migration; ++i) {
      auto* current = nodes[i]->As<GraphTestNode>();
      EXPECT_EQ(current->value(), i);
      if (i > 0) {
        ASSERT_EQ(current->edges.size(), 1);
        EXPECT_EQ(current->edges[0], nodes[i - 1]);
      }
    }
    graph = std::move(*result);
  }
}

}  // namespace
}  // namespace google::spanner::emulator::backend::test
