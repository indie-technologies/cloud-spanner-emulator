//
// Copyright 2020 Google LLC
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
//

#include "backend/transaction/read_only_transaction.h"

#include <memory>
#include <thread>

#include "absl/status/status.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "backend/datamodel/key_range.h"
#include "backend/locking/manager.h"
#include "backend/schema/catalog/versioned_catalog.h"
#include "backend/storage/in_memory_storage.h"
#include "backend/transaction/options.h"
#include "common/clock.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"

namespace google::spanner::emulator::backend {
namespace {

using googlesql_base::testing::StatusIs;

class ReadOnlyTransactionTest : public testing::Test {
 protected:
  std::unique_ptr<ReadOnlyTransaction> Reader(ReadOnlyOptions options = {}) {
    return std::make_unique<ReadOnlyTransaction>(
        options, next_id_++, &clock_, &storage_, &manager_, &catalog_);
  }
  std::unique_ptr<LockHandle> Writer() {
    return manager_.CreateHandle(next_id_++, nullptr, 1);
  }
  void Acquire(LockHandle* handle) {
    handle->EnqueueLock(LockRequest(LockMode::kExclusive, "", KeyRange::All(), {}));
  }

  Clock clock_;
  InMemoryStorage storage_;
  LockManager manager_{&clock_};
  VersionedCatalog catalog_;
  TransactionID next_id_ = 1;
};

TEST_F(ReadOnlyTransactionTest, StrongReadPinsCurrentSchemaAndTimestamp) {
  const auto before = clock_.Now();
  auto reader = Reader();
  GOOGLESQL_ASSERT_OK(reader->status());
  EXPECT_GE(reader->read_timestamp(), before);
  EXPECT_LE(reader->read_timestamp(), clock_.Now());
  EXPECT_EQ(reader->schema(), catalog_.GetLatestSchema());
  const auto timestamp = reader->read_timestamp();
  GOOGLESQL_EXPECT_OK(reader->GuardedCall([] { return absl::OkStatus(); }));
  EXPECT_EQ(reader->read_timestamp(), timestamp);
}

TEST_F(ReadOnlyTransactionTest, RejectsEveryHistoricalTimestampBound) {
  for (const auto bound : {TimestampBound::kExactTimestamp,
                          TimestampBound::kExactStaleness,
                          TimestampBound::kMaxStaleness,
                          TimestampBound::kMinTimestamp}) {
    auto reader = Reader(ReadOnlyOptions{.bound = bound});
    EXPECT_THAT(reader->status(), StatusIs(absl::StatusCode::kUnimplemented));
    EXPECT_THAT(reader->GuardedCall([] { return absl::OkStatus(); }),
                StatusIs(absl::StatusCode::kUnimplemented));
  }
  auto writer = Writer();
  Acquire(writer.get());
  GOOGLESQL_EXPECT_OK(writer->Wait());
  writer->UnlockAll();
}

TEST_F(ReadOnlyTransactionTest, ReadersAndWriterRemainUsableTogether) {
  auto first = Reader();
  auto second = Reader();
  auto writer = Writer();
  Acquire(writer.get());
  GOOGLESQL_ASSERT_OK(writer->Wait());
  GOOGLESQL_EXPECT_OK(first->status());
  GOOGLESQL_EXPECT_OK(second->status());
  GOOGLESQL_EXPECT_OK(first->GuardedCall([] { return absl::OkStatus(); }));
  first->Close();
  GOOGLESQL_EXPECT_OK(second->GuardedCall([] { return absl::OkStatus(); }));
  writer->UnlockAll();
}

TEST_F(ReadOnlyTransactionTest, ExistingWriterAllowsNewReaders) {
  auto writer = Writer();
  Acquire(writer.get());
  GOOGLESQL_ASSERT_OK(writer->Wait());
  auto reader = Reader();
  GOOGLESQL_EXPECT_OK(reader->status());
  EXPECT_NE(reader->schema(), nullptr);
  writer->UnlockAll();
}

TEST_F(ReadOnlyTransactionTest, ActiveRequestAllowsOtherReadersAndWriter) {
  auto reader = Reader();
  GOOGLESQL_EXPECT_OK(reader->GuardedCall([&] {
    std::thread competitor([&] {
      auto writer = Writer();
      Acquire(writer.get());
      GOOGLESQL_EXPECT_OK(writer->Wait());
      auto other = Reader();
      GOOGLESQL_EXPECT_OK(other->status());
      GOOGLESQL_EXPECT_OK(other->GuardedCall([] { return absl::OkStatus(); }));
      writer->UnlockAll();
    });
    competitor.join();
    return absl::OkStatus();
  }));
  GOOGLESQL_EXPECT_OK(reader->status());
}

TEST_F(ReadOnlyTransactionTest, RegistrationWaitsForCommitPublication) {
  absl::Notification started;
  absl::Notification finished;
  manager_.snapshot_mutex()->Lock();
  std::thread reader([&] {
    started.Notify();
    GOOGLESQL_EXPECT_OK(Reader()->status());
    finished.Notify();
  });
  started.WaitForNotification();
  EXPECT_FALSE(finished.WaitForNotificationWithTimeout(absl::Milliseconds(20)));
  manager_.snapshot_mutex()->Unlock();
  reader.join();
  EXPECT_TRUE(finished.HasBeenNotified());
}

TEST_F(ReadOnlyTransactionTest, SchemaUpdateWaitsForActiveSqlRequest) {
  auto reader = Reader();
  absl::Notification started;
  absl::Notification finished;
  std::thread ddl;
  GOOGLESQL_EXPECT_OK(reader->GuardedCall([&] {
    ddl = std::thread([&] {
      started.Notify();
      absl::MutexLock lock(manager_.schema_mutex());
      finished.Notify();
    });
    started.WaitForNotification();
    EXPECT_FALSE(
        finished.WaitForNotificationWithTimeout(absl::Milliseconds(20)));
    return absl::OkStatus();
  }));
  ddl.join();
  EXPECT_TRUE(finished.HasBeenNotified());
  GOOGLESQL_EXPECT_OK(reader->status());
}

TEST_F(ReadOnlyTransactionTest, ClosePermanentlyRejectsFurtherRequests) {
  auto reader = Reader();
  GOOGLESQL_EXPECT_OK(reader->GuardedCall([&] {
    reader->Close();
    auto writer = Writer();
    Acquire(writer.get());
    GOOGLESQL_EXPECT_OK(writer->Wait());
    writer->UnlockAll();
    return absl::OkStatus();
  }));
  EXPECT_FALSE(reader->status().ok());
  bool called = false;
  EXPECT_FALSE(reader
                   ->GuardedCall([&] {
                     called = true;
                     return absl::OkStatus();
                   })
                   .ok());
  EXPECT_FALSE(called);
  std::unique_ptr<RowCursor> cursor;
  EXPECT_FALSE(reader->Read(ReadArg{}, &cursor).ok());
}

TEST_F(ReadOnlyTransactionTest, InvalidQueryDoesNotInvalidateSnapshot) {
  auto reader = Reader();
  EXPECT_THAT(reader->GuardedCall([] {
    return absl::InvalidArgumentError("invalid query");
  }), StatusIs(absl::StatusCode::kInvalidArgument));
  GOOGLESQL_EXPECT_OK(reader->status());
  GOOGLESQL_EXPECT_OK(reader->GuardedCall([] { return absl::OkStatus(); }));
}

}  // namespace
}  // namespace google::spanner::emulator::backend
