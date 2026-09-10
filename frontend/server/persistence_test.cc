// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0.
#include "frontend/server/persistence.h"

#include <filesystem>
#include <fstream>
#include <thread>

#include "backend/persistence/files.h"
#include "frontend/server/server.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"

namespace google::spanner::emulator::frontend {
namespace {
using googlesql::values::Int64;
constexpr char kInstance[] = "projects/p/instances/i";
constexpr char kDatabase[] = "projects/p/instances/i/databases/db";
class PersistenceTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string name = testing::TempDir() + "/spanner-persistence-XXXXXX";
    ASSERT_NE(mkdtemp(name.data()), nullptr);
    dir = name;
    path = dir + "/state";
  }
  void TearDown() override { std::filesystem::remove_all(dir); }
  absl::Status Populate(ServerEnv* env) {
    admin::instance::v1::Instance instance;
    instance.set_display_name("Development");
    instance.set_config("projects/p/instanceConfigs/emulator-config");
    instance.set_processing_units(1000);
    (*instance.mutable_labels())["owner"] = "test";
    GOOGLESQL_RETURN_IF_ERROR(
        env->instance_manager()->CreateInstance(kInstance, instance).status());
    GOOGLESQL_ASSIGN_OR_RETURN(
        auto db, env->database_manager()->CreateDatabase(
                     kDatabase,
                     {.statements = {
                          "CREATE TABLE T (id INT64, v INT64) PRIMARY KEY(id)",
                          "CREATE SEQUENCE S "
                          "OPTIONS(sequence_kind='bit_reversed_positive')"}}));
    return Write(db->backend(), 1);
  }
  absl::Status Write(backend::Database* db, int value) {
    GOOGLESQL_ASSIGN_OR_RETURN(auto writer,
                               db->CreateReadWriteTransaction({}, {}));
    backend::Mutation mutation;
    mutation.AddWriteOp(backend::MutationOpType::kInsertOrUpdate, "T",
                        {"id", "v"}, {{Int64(1), Int64(value)}});
    GOOGLESQL_RETURN_IF_ERROR(writer->Write(mutation));
    return writer->Commit();
  }
  std::string Bytes(const std::string& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
  }
  void Put(const std::string& file, const std::string& contents) {
    std::ofstream output(file, std::ios::binary);
    output.write(contents.data(), contents.size());
  }
  std::string dir, path;
};

TEST_F(PersistenceTest, MissingFileRoundTripMetadataAndCleanCheckpoints) {
  std::string instance_bytes, partition_bytes;
  {
    ServerEnv env;
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto state, Persistence::Open(&env, path));
    EXPECT_TRUE(env.database_manager()->Capture().entries.empty());
    EXPECT_FALSE(std::filesystem::exists(path));
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(bool initial_write, state->Checkpoint());
    EXPECT_FALSE(initial_write);
    EXPECT_FALSE(std::filesystem::exists(path));
    GOOGLESQL_ASSERT_OK(Populate(&env));
    admin::instance::v1::Instance instance;
    env.instance_manager()->Capture().entries[0]->ToProto(&instance);
    instance_bytes = instance.SerializeAsString();
    admin::instance::v1::InstancePartition partition;
    partition.set_node_count(1);
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        auto created_partition,
        env.instance_partition_manager()->CreateInstancePartition(
            "projects/p/instances/i/instancePartitions/part", partition));
    created_partition->ToProto(&partition);
    partition_bytes = partition.SerializeAsString();
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(bool written, state->Checkpoint());
    EXPECT_TRUE(written);
    auto timestamp = std::filesystem::last_write_time(path);
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(written, state->Checkpoint());
    EXPECT_FALSE(written);
    EXPECT_EQ(timestamp, std::filesystem::last_write_time(path));
    // A no-op transaction doesn't make a clean database dirty.
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        auto db, env.database_manager()->GetDatabase(kDatabase));
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        auto writer, db->backend()->CreateReadWriteTransaction({}, {}));
    GOOGLESQL_ASSERT_OK(writer->Commit());
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(written, state->Checkpoint());
    EXPECT_FALSE(written);
    // Sequence allocations alone do.
    GOOGLESQL_ASSERT_OK(db->backend()
                            ->GetLatestSchema()
                            ->FindSequence("S")
                            ->GetNextSequenceValue());
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(written, state->Checkpoint());
    EXPECT_TRUE(written);
  }
  ServerEnv restored;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto state,
                                 Persistence::Open(&restored, path));
  admin::instance::v1::Instance instance;
  restored.instance_manager()->Capture().entries[0]->ToProto(&instance);
  EXPECT_EQ(instance.SerializeAsString(), instance_bytes);
  admin::instance::v1::InstancePartition partition;
  restored.instance_partition_manager()->Capture().entries[0]->ToProto(
      &partition);
  EXPECT_EQ(partition.SerializeAsString(), partition_bytes);
  EXPECT_EQ(restored.database_manager()->Capture().entries.size(), 1);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(bool written, state->Checkpoint());
  EXPECT_FALSE(written);
}

TEST_F(PersistenceTest, DeletedAndRecreatedResourcesPersist) {
  {
    ServerEnv env;
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto state, Persistence::Open(&env, path));
    GOOGLESQL_ASSERT_OK(Populate(&env));
    GOOGLESQL_ASSERT_OK(state->Checkpoint());
    GOOGLESQL_ASSERT_OK(env.database_manager()->DeleteDatabase(kDatabase));
    env.instance_manager()->DeleteInstance(kInstance);
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(bool written, state->Checkpoint());
    EXPECT_TRUE(written);
  }
  {
    ServerEnv env;
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto state, Persistence::Open(&env, path));
    EXPECT_TRUE(env.instance_manager()->Capture().entries.empty());
    GOOGLESQL_ASSERT_OK(Populate(&env));
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(bool written, state->Checkpoint());
    EXPECT_TRUE(written);
  }
  ServerEnv env;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto state, Persistence::Open(&env, path));
  EXPECT_EQ(env.database_manager()->Capture().entries.size(), 1);
}

TEST_F(PersistenceTest, PeriodicCheckpointsAndRetryAfterIoFailure) {
  ServerEnv env;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto state, Persistence::Open(&env, path));
  GOOGLESQL_ASSERT_OK(Populate(&env));
  state->Start(absl::Milliseconds(20));
  for (int i = 0; i < 200 && !std::filesystem::exists(path); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  state->Stop();
  ASSERT_TRUE(std::filesystem::exists(path));
  auto bytes = Bytes(path);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db, env.database_manager()->GetDatabase(kDatabase));
  GOOGLESQL_ASSERT_OK(Write(db->backend(), 2));
  std::filesystem::rename(dir, dir + "-moved");
  auto failed = state->Checkpoint();
  EXPECT_FALSE(failed.ok());
  EXPECT_EQ(Bytes(dir + "-moved/state"), bytes);
  std::filesystem::rename(dir + "-moved", dir);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(bool written, state->Checkpoint());
  EXPECT_TRUE(written);
  EXPECT_NE(Bytes(path), bytes);
}

TEST_F(PersistenceTest, InterruptedTemporaryFilesDoNotReplaceLastCheckpoint) {
  {
    ServerEnv env;
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto state, Persistence::Open(&env, path));
    GOOGLESQL_ASSERT_OK(Populate(&env));
    GOOGLESQL_ASSERT_OK(state->Checkpoint());
  }
  auto bytes = Bytes(path);
  for (size_t length :
       {size_t(0), size_t(15), bytes.size() / 2, bytes.size()}) {
    Put(path + ".tmp.interrupted", bytes.substr(0, length));
    ServerEnv env;
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto state, Persistence::Open(&env, path));
    EXPECT_EQ(env.database_manager()->Capture().entries.size(), 1);
    EXPECT_EQ(Bytes(path), bytes);
  }
}

TEST_F(PersistenceTest, InvalidFilesFailWithoutOverwritingDestination) {
  {
    ServerEnv env;
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto state, Persistence::Open(&env, path));
    GOOGLESQL_ASSERT_OK(Populate(&env));
    GOOGLESQL_ASSERT_OK(state->Checkpoint());
  }
  auto valid = Bytes(path);
  std::vector<std::string> invalid{"", "not a snapshot", valid.substr(0, 23),
                                   valid.substr(0, valid.size() - 1),
                                   valid + "trailing"};
  auto incompatible = valid;
  incompatible[8] = 99;
  invalid.push_back(incompatible);
  auto corrupted = valid;
  corrupted.back() ^= 1;
  invalid.push_back(corrupted);
  for (const auto& bytes : invalid) {
    Put(path, bytes);
    ServerEnv env;
    auto result = Persistence::Open(&env, path);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(Bytes(path), bytes);
    // Also verify the actual launch path rejects this before listening.
    EXPECT_EQ(
        Server::Create({.server_address = "127.0.0.1:0", .state_file = path}),
        nullptr);
  }
  Put(path, valid);
  persistence::Snapshot snapshot;
  GOOGLESQL_ASSERT_OK(persistence::ReadSnapshotFile(path, &snapshot));
  snapshot.mutable_databases(0)->mutable_tables(0)->set_name("missing");
  GOOGLESQL_ASSERT_OK(persistence::WriteSnapshotFile(path, snapshot));
  auto malformed = Bytes(path);
  ServerEnv env;
  EXPECT_FALSE(Persistence::Open(&env, path).ok());
  EXPECT_EQ(Bytes(path), malformed);
}

TEST_F(PersistenceTest, FileLockPreventsTwoEmulatorsOwningTheSameState) {
  ServerEnv first, second;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto state, Persistence::Open(&first, path));
  EXPECT_FALSE(Persistence::Open(&second, path).ok());
  state.reset();
  GOOGLESQL_ASSERT_OK(Persistence::Open(&second, path));
}

TEST_F(PersistenceTest, ServerShutdownSavesAndRestartRestores) {
  {
    auto server =
        Server::Create({.server_address = "127.0.0.1:0", .state_file = path});
    ASSERT_NE(server, nullptr);
    GOOGLESQL_ASSERT_OK(Populate(server->env()));
    GOOGLESQL_ASSERT_OK(server->Shutdown());
  }
  {
    auto server =
        Server::Create({.server_address = "127.0.0.1:0", .state_file = path});
    ASSERT_NE(server, nullptr);
    EXPECT_EQ(server->env()->database_manager()->Capture().entries.size(), 1);
    GOOGLESQL_ASSERT_OK(server->Shutdown());
  }
  auto ephemeral = Server::Create({.server_address = "127.0.0.1:0"});
  ASSERT_NE(ephemeral, nullptr);
  EXPECT_TRUE(ephemeral->env()->database_manager()->Capture().entries.empty());
  GOOGLESQL_ASSERT_OK(ephemeral->Shutdown());
}
}  // namespace
}  // namespace google::spanner::emulator::frontend
