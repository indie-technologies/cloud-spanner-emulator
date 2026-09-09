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

#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "backend/datamodel/key_range.h"
#include "backend/locking/manager.h"
#include "backend/schema/catalog/versioned_catalog.h"
#include "backend/storage/in_memory_storage.h"
#include "backend/transaction/options.h"
#include "common/clock.h"

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

TEST_F(ReadOnlyTransactionTest, IdleReaderIsAbortedPermanentlyOnContention) {
  auto reader = Reader();
  GOOGLESQL_ASSERT_OK(reader->status());
  auto writer = Writer();
  Acquire(writer.get());
  GOOGLESQL_ASSERT_OK(writer->Wait());
  EXPECT_THAT(reader->status(), StatusIs(absl::StatusCode::kAborted));
  writer->UnlockAll();
  bool called = false;
  EXPECT_THAT(reader->GuardedCall([&] {
    called = true;
    return absl::OkStatus();
  }), StatusIs(absl::StatusCode::kAborted));
  EXPECT_FALSE(called);
  std::unique_ptr<RowCursor> cursor;
  EXPECT_THAT(reader->Read(ReadArg{}, &cursor),
              StatusIs(absl::StatusCode::kAborted));
}

TEST_F(ReadOnlyTransactionTest, SecondReaderAlsoAbortsIdleReader) {
  auto first = Reader();
  auto second = Reader();
  GOOGLESQL_EXPECT_OK(second->status());
  EXPECT_THAT(first->status(), StatusIs(absl::StatusCode::kAborted));
  // Closing the old transaction must not release its successor's lock.
  first->Close();
  GOOGLESQL_EXPECT_OK(second->GuardedCall([&] {
    auto writer = Writer();
    Acquire(writer.get());
    EXPECT_THAT(writer->Wait(), StatusIs(absl::StatusCode::kAborted));
    return absl::OkStatus();
  }));
}

TEST_F(ReadOnlyTransactionTest, ActiveRequestExcludesReadersWritersAndDdl) {
  auto reader = Reader();
  GOOGLESQL_ASSERT_OK(reader->status());
  GOOGLESQL_EXPECT_OK(reader->GuardedCall([&] {
    // A second thread tries to take ownership while SQL/streaming is active.
    std::thread competitor([&] {
      auto writer = Writer();
      Acquire(writer.get());
      EXPECT_THAT(writer->Wait(), StatusIs(absl::StatusCode::kAborted));
      auto other_reader = Reader();
      EXPECT_THAT(other_reader->status(), StatusIs(absl::StatusCode::kAborted));
    });
    competitor.join();
    // A nested storage Read guard must not prematurely unpin the request.
    GOOGLESQL_EXPECT_OK(reader->GuardedCall([] { return absl::OkStatus(); }));
    auto writer = Writer();
    Acquire(writer.get());
    EXPECT_THAT(writer->Wait(), StatusIs(absl::StatusCode::kAborted));
    return absl::OkStatus();
  }));
  GOOGLESQL_EXPECT_OK(reader->status());
}

TEST_F(ReadOnlyTransactionTest, ExistingWriterPreventsReaderInitialization) {
  auto writer = Writer();
  Acquire(writer.get());
  GOOGLESQL_ASSERT_OK(writer->Wait());
  auto reader = Reader();
  EXPECT_THAT(reader->status(), StatusIs(absl::StatusCode::kAborted));
  EXPECT_EQ(reader->schema(), nullptr);
  writer->UnlockAll();
}

TEST_F(ReadOnlyTransactionTest, DestructionAndCloseReleaseDatabaseOwnership) {
  {
    auto reader = Reader();
    GOOGLESQL_ASSERT_OK(reader->status());
  }
  auto writer = Writer();
  Acquire(writer.get());
  GOOGLESQL_ASSERT_OK(writer->Wait());
  writer->UnlockAll();
  auto reader = Reader();
  reader->Close();
  EXPECT_FALSE(reader->status().ok());
  Acquire(writer.get());
  GOOGLESQL_EXPECT_OK(writer->Wait());
  writer->UnlockAll();
}

TEST_F(ReadOnlyTransactionTest, CloseDuringRequestDefersUnlockUntilRequestEnds) {
  auto reader = Reader();
  GOOGLESQL_EXPECT_OK(reader->GuardedCall([&] {
    reader->Close();
    auto writer = Writer();
    Acquire(writer.get());
    EXPECT_THAT(writer->Wait(), StatusIs(absl::StatusCode::kAborted));
    return absl::OkStatus();
  }));
  auto writer = Writer();
  Acquire(writer.get());
  GOOGLESQL_EXPECT_OK(writer->Wait());
  writer->UnlockAll();
}

TEST_F(ReadOnlyTransactionTest, FailedRequestDoesNotLeaveAnUnabortableReader) {
  auto reader = Reader();
  EXPECT_THAT(reader->GuardedCall([] {
    return absl::InvalidArgumentError("invalid query");
  }), StatusIs(absl::StatusCode::kInvalidArgument));
  auto writer = Writer();
  Acquire(writer.get());
  GOOGLESQL_EXPECT_OK(writer->Wait());
  EXPECT_THAT(reader->status(), StatusIs(absl::StatusCode::kAborted));
  writer->UnlockAll();
}

}  // namespace
}  // namespace google::spanner::emulator::backend
