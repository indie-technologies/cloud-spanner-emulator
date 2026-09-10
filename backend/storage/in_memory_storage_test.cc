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

#include "backend/storage/in_memory_storage.h"

#include <map>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/datamodel/key_range.h"
#include "backend/storage/iterator.h"
#include "gmock/gmock.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Bool;
using googlesql::values::Int64;
using googlesql::values::String;

class InMemoryStorageTest : public testing::Test {
 protected:
  const TableID kTableId0 = "test_table:0";
  const TableID kTableId1 = "test_table:1";
  const ColumnID kColumnID = "test_column:0";
  const KeyRange kKeyRange0To5 =
      KeyRange::ClosedOpen(Key({Int64(0)}), Key({Int64(5)}));
  InMemoryStorage storage_;
  std::unique_ptr<StorageIterator> itr_;
};

TEST_F(InMemoryStorageTest,
       SnapshotsRetainRowsAcrossUpdatesDeletesAndReinserts) {
  const auto now = absl::Now();
  const Key key({Int64(1)});
  const ColumnID other = "other";
  GOOGLESQL_ASSERT_OK(storage_.Write(now, kTableId0, key, {kColumnID, other},
                                     {Int64(10), Int64(20)}));
  auto first = storage_.CreateSnapshot();
  GOOGLESQL_ASSERT_OK(
      storage_.Write(now, kTableId0, key, {kColumnID}, {Int64(30)}));
  auto second = storage_.CreateSnapshot();
  GOOGLESQL_ASSERT_OK(storage_.Delete(now, kTableId0, KeyRange::All()));
  GOOGLESQL_ASSERT_OK(
      storage_.Write(now, kTableId0, key, {other}, {Int64(40)}));
  std::vector<googlesql::Value> values;
  GOOGLESQL_ASSERT_OK(
      first->Lookup(now, kTableId0, key, {kColumnID, other}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Int64(10), Int64(20)));
  GOOGLESQL_ASSERT_OK(
      second->Lookup(now, kTableId0, key, {kColumnID, other}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Int64(30), Int64(20)));
  auto cloned = first->CreateSnapshot();
  first.reset();
  GOOGLESQL_ASSERT_OK(
      cloned->Lookup(now, kTableId0, key, {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Int64(10)));
  GOOGLESQL_ASSERT_OK(
      storage_.Lookup(now, kTableId0, key, {kColumnID, other}, &values));
  EXPECT_FALSE(values[0].is_valid());
  EXPECT_EQ(values[1], Int64(40));
}

TEST_F(InMemoryStorageTest, SnapshotDistinguishesMissingAndEmptyRows) {
  const auto now = absl::Now();
  const Key key({Int64(1)});
  auto missing = storage_.CreateSnapshot();
  GOOGLESQL_ASSERT_OK(storage_.Write(now, kTableId0, key, {}, {}));
  auto empty = storage_.CreateSnapshot();
  GOOGLESQL_ASSERT_OK(
      storage_.Write(now, kTableId0, key, {kColumnID}, {Int64(1)}));
  std::vector<googlesql::Value> values;
  EXPECT_EQ(missing->Lookup(now, kTableId0, key, {}, nullptr).code(),
            absl::StatusCode::kNotFound);
  GOOGLESQL_ASSERT_OK(empty->Lookup(now, kTableId0, key, {kColumnID}, &values));
  ASSERT_EQ(values.size(), 1);
  EXPECT_FALSE(values[0].is_valid());
  EXPECT_EQ(empty->Write(now, kTableId0, key, {}, {}).code(),
            absl::StatusCode::kFailedPrecondition);
  EXPECT_EQ(empty->Delete(now, kTableId0, KeyRange::All()).code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST_F(InMemoryStorageTest, SnapshotsSurvivePhysicalSchemaCleanup) {
  const auto now = absl::Now();
  const Key key({Int64(1)});
  for (const auto& table : {kTableId0, kTableId1}) {
    GOOGLESQL_ASSERT_OK(
        storage_.Write(now, table, key, {kColumnID}, {Int64(7)}));
  }
  auto snapshot = storage_.CreateSnapshot();
  storage_.SetVersionRetentionPeriod(absl::ZeroDuration());
  storage_.MarkDroppedTable(now, kTableId0);
  storage_.MarkDroppedColumn(now, kTableId1, kColumnID);
  storage_.CleanUpDeletedTables(now + absl::Seconds(1));
  storage_.CleanUpDeletedColumns(now + absl::Seconds(1));
  for (const auto& table : {kTableId0, kTableId1}) {
    std::vector<googlesql::Value> values;
    GOOGLESQL_ASSERT_OK(
        snapshot->Lookup(now, table, key, {kColumnID}, &values));
    EXPECT_THAT(values, testing::ElementsAre(Int64(7)));
    GOOGLESQL_ASSERT_OK(
        snapshot->Read(now, table, KeyRange::All(), {kColumnID}, &itr_));
    ASSERT_TRUE(itr_->Next());
    EXPECT_EQ(itr_->ColumnValue(0), Int64(7));
    EXPECT_FALSE(itr_->Next());
  }
}

TEST_F(InMemoryStorageTest, SnapshotScansMatchIndependentCopiedModels) {
  const auto now = absl::Now();
  std::mt19937 random(41627);
  using Model = std::map<int, int>;
  Model current;
  std::vector<std::pair<std::unique_ptr<Storage>, Model>> snapshots;
  auto check = [&](Storage* storage, const Model& expected, int begin,
                   int end) {
    std::unique_ptr<StorageIterator> rows;
    GOOGLESQL_ASSERT_OK(storage->Read(
        now, kTableId0,
        KeyRange::ClosedOpen(Key({Int64(begin)}), Key({Int64(end)})),
        {kColumnID}, &rows));
    for (auto it = expected.lower_bound(begin); it != expected.lower_bound(end);
         ++it) {
      ASSERT_TRUE(rows->Next());
      EXPECT_EQ(rows->Key(), Key({Int64(it->first)}));
      EXPECT_EQ(rows->ColumnValue(0), Int64(it->second));
    }
    EXPECT_FALSE(rows->Next());
  };
  for (int i = 0; i < 500; ++i) {
    const int key = random() % 12;
    switch (random() % 4) {
      case 0:
        if (snapshots.size() == 4) snapshots.erase(snapshots.begin());
        snapshots.emplace_back(storage_.CreateSnapshot(), current);
        break;
      case 1: {
        const int end = key + random() % (13 - key);
        GOOGLESQL_ASSERT_OK(storage_.Delete(
            now, kTableId0,
            KeyRange::ClosedOpen(Key({Int64(key)}), Key({Int64(end)}))));
        current.erase(current.lower_bound(key), current.lower_bound(end));
        break;
      }
      default:
        GOOGLESQL_ASSERT_OK(storage_.Write(now, kTableId0, Key({Int64(key)}),
                                           {kColumnID}, {Int64(i)}));
        current[key] = i;
        break;
    }
    check(&storage_, current, 0, 12);
    for (const auto& [snapshot, expected] : snapshots) {
      check(snapshot.get(), expected, 0, 12);
      check(snapshot.get(), expected, 3, 9);
    }
  }
}

TEST_F(InMemoryStorageTest, ReadersKeepTheirValuesDuringConcurrentWrites) {
  const auto now = absl::Now();
  const Key key({Int64(1)});
  GOOGLESQL_ASSERT_OK(
      storage_.Write(now, kTableId0, key, {kColumnID}, {Int64(-1)}));
  auto snapshot = storage_.CreateSnapshot();
  std::thread writer([&] {
    for (int i = 0; i < 2000; ++i) {
      GOOGLESQL_EXPECT_OK(
          storage_.Write(now, kTableId0, key, {kColumnID}, {Int64(i)}));
    }
  });
  for (int i = 0; i < 2000; ++i) {
    std::vector<googlesql::Value> values;
    GOOGLESQL_EXPECT_OK(
        snapshot->Lookup(now, kTableId0, key, {kColumnID}, &values));
    EXPECT_THAT(values, testing::ElementsAre(Int64(-1)));
  }
  writer.join();
}

TEST_F(InMemoryStorageTest, LookupByTable) {
  absl::Time t0 = absl::Now();

  // Write into 2 tables.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId1, Key({Int64(10)}), {kColumnID},
                           {String("value-10")}));

  // Read from the 2 tables.
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-1")));
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(t0, kTableId1, Key({Int64(10)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-10")));
}

TEST_F(InMemoryStorageTest, ReadByTable) {
  absl::Time t0 = absl::Now();

  // Write into 2 tables.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId1, Key({Int64(4)}), {kColumnID},
                           {String("value-10")}));

  // Read from the 2 table.
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->NumColumns(), 1);
  EXPECT_EQ(itr_->ColumnValue(0), String("value-1"));
  EXPECT_EQ(itr_->Key(), Key({Int64(1)}));
  EXPECT_FALSE(itr_->Next());

  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId1, kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->NumColumns(), 1);
  EXPECT_EQ(itr_->ColumnValue(0), String("value-10"));
  EXPECT_EQ(itr_->Key(), Key({Int64(4)}));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, ReadRangeFromSingleTable) {
  absl::Time t0 = absl::Now();

  // Write into table.
  for (int i = 0; i < 5; ++i) {
    GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(i)}), {kColumnID},
                             {String(absl::StrCat("value-", i))}));
  }

  // Read from the table.
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(itr_->Next());
    EXPECT_EQ(itr_->NumColumns(), 1);
    EXPECT_EQ(itr_->ColumnValue(0), String(absl::StrCat("value-", i)));
    EXPECT_EQ(itr_->Key(), Key({Int64(i)}));
  }
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, WritesReplaceCurrentCellValues) {
  const auto now = absl::Now();
  const Key key({Int64(1)});
  for (int i = 0; i < 1000; ++i) {
    GOOGLESQL_ASSERT_OK(storage_.Write(now + absl::Seconds(i), kTableId0, key,
                                      {kColumnID}, {Int64(i)}));
  }
  std::vector<googlesql::Value> values;
  // Storage timestamps are compatibility parameters, not historical reads.
  GOOGLESQL_ASSERT_OK(storage_.Lookup(now, kTableId0, key, {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Int64(999)));
}

TEST_F(InMemoryStorageTest, ReadReturnsCurrentValues) {
  const auto now = absl::Now();
  const Key key({Int64(1)});
  GOOGLESQL_ASSERT_OK(storage_.Write(now, kTableId0, key, {kColumnID}, {Int64(1)}));
  GOOGLESQL_ASSERT_OK(storage_.Write(now + absl::Seconds(1), kTableId0, key,
                                    {kColumnID}, {Int64(2)}));
  GOOGLESQL_ASSERT_OK(storage_.Read(now, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  ASSERT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->ColumnValue(0), Int64(2));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupInvalidTableReturnsNotFoundError) {
  absl::Time t0 = absl::Now();

  // Lookup a table_id in empty storage.
  std::vector<googlesql::Value> values;
  EXPECT_THAT(
      storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));

  // Lookup invalid table_id in storage.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  EXPECT_THAT(storage_.Lookup(t0, "invalid-table_id_", Key({Int64(1)}),
                              {kColumnID}, &values),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(InMemoryStorageTest, ReadInvalidTableReturnsEmptyResult) {
  absl::Time t0 = absl::Now();

  // Read a table_id in empty storage.
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());

  // Read invalid table_id in storage.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, "invalid-table_id_", kKeyRange0To5, {kColumnID},
                          &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupMissingKeyReturnsNotFound) {
  absl::Time t0 = absl::Now();

  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  EXPECT_THAT(
      storage_.Lookup(t0, kTableId0, Key({Int64(100)}), {kColumnID}, &values),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(InMemoryStorageTest, ReadMissingKeyReturnsEmptyItr) {
  absl::Time t0 = absl::Now();
  KeyRange key_range = KeyRange::ClosedOpen(Key({Int64(10)}), Key({Int64(50)}));

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, key_range, {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, ReadEmptyKeyRangeReturnsEmptyItr) {
  absl::Time t0 = absl::Now();
  KeyRange key_range = KeyRange::Empty();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, key_range, {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());

  GOOGLESQL_EXPECT_OK(storage_.Read(
      t0, kTableId0,
      KeyRange(EndpointType::kClosed, Key(), EndpointType::kOpen, Key()), {},
      &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupByMissingColumnReturnsInvalidValues) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));

  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(t0 + absl::Nanoseconds(5), kTableId0,
                            Key({Int64(1)}), {"invalid_kColumnID"}, &values));
  EXPECT_FALSE(values.empty());
  EXPECT_FALSE(values[0].is_valid());
}

TEST_F(InMemoryStorageTest, ReadByMissingColumnReturnsInvalidValues) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));

  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, kKeyRange0To5, {"invalid_kColumnID"},
                          &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_FALSE(itr_->ColumnValue(0).is_valid());
  EXPECT_EQ(itr_->Key(), Key({Int64(1)}));
}

TEST_F(InMemoryStorageTest, LookupWithoutColumns) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {}, &values));
  EXPECT_TRUE(values.empty());
}

TEST_F(InMemoryStorageTest, ReadWithoutColumns) {
  absl::Time write_ts = absl::Now();
  absl::Time lookup_ts = write_ts + absl::Seconds(1);
  Key key({String("key"), Int64(1)});

  for (int i = 0; i < 5; i++) {
    Key key({String("key"), Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }

  std::unique_ptr<StorageIterator> itr;
  GOOGLESQL_EXPECT_OK(
      storage_.Read(lookup_ts, kTableId0, KeyRange::Point(key), {}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->NumColumns(), 0);
  EXPECT_EQ(itr_->Key(), key);
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupWithNullValuesReturnsInternalError) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  EXPECT_THAT(storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                              /*values =*/nullptr),
              googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
}

TEST_F(InMemoryStorageTest, WriteWithEmptyKeyAndColumns) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key(), {}, {}));
}

TEST_F(InMemoryStorageTest, WriteWithEmptyColumns) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {}, {}));
}

TEST_F(InMemoryStorageTest, DeleteFromNonExistentTable) {
  absl::Time t0 = absl::Now();
  Key key({Int64(1)});

  GOOGLESQL_EXPECT_OK(storage_.Delete(t0, kTableId0, KeyRange::Point(key)));
}

TEST_F(InMemoryStorageTest, DuplicateDeleteReturnsOk) {
  Key key({Int64(1)});
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);

  GOOGLESQL_EXPECT_OK(
      storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Point(key)));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Point(key)));
}

TEST_F(InMemoryStorageTest, DeleteEmptyRangeWillDeleteNothing) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = absl::Now();
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(1)}), {kColumnID},
                           {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange()));
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(1)}), {kColumnID},
                            &values));
  EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
}

TEST_F(InMemoryStorageTest, DeleteLargeRangeFromSparseTable) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  // Write sparse keys.
  for (int j = 0; j < 5; j++) {
    GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(5 * j)}),
                             {kColumnID}, {Bool(true)}));
  }

  // Delete key range [0, 50).
  KeyRange key_range(KeyRange::ClosedOpen(Key({Int64(0)}), Key({Int64(50)})));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, key_range));

  for (int j = 0; j < 5; j++) {
    std::vector<googlesql::Value> values;
    EXPECT_THAT(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(5 * j)}),
                                {kColumnID}, &values),
                googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  }
}

TEST_F(InMemoryStorageTest, DeleteRangeNotInTableWillDeleteNothing) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  // Write key range [0, 5).
  for (int i = 0; i < 5; i++) {
    GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(i)}), {kColumnID},
                             {Bool(true)}));
  }

  // Delete key range [10, 100).
  KeyRange key_range(KeyRange::ClosedOpen(Key({Int64(10)}), Key({Int64(100)})));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, key_range));

  // Lookup for all existing keys [0, 5) should succeed.
  for (int i = 0; i < 5; i++) {
    std::vector<googlesql::Value> values;
    GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(i)}),
                              {kColumnID}, &values));
    EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
    values.clear();
  }
}

TEST_F(InMemoryStorageTest, DeletePartialKeyRangeFromTable) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  // Write keys in the range [10, 15].
  for (int i = 10; i <= 15; i++) {
    GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(i)}), {kColumnID},
                             {Bool(true)}));
  }

  // Delete keys in the range [6, 12).
  KeyRange key_range(KeyRange::ClosedOpen(Key({Int64(6)}), Key({Int64(12)})));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, key_range));

  // Lookup after delete should return invalid values for keys range [10, 12).
  std::vector<googlesql::Value> values;
  for (int i = 10; i < 12; i++) {
    EXPECT_THAT(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(i)}),
                                {kColumnID}, &values),
                googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  }
  // Lookup for range [12, 15] should return valid values.
  for (int i = 12; i <= 15; i++) {
    std::vector<googlesql::Value> values;
    GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(i)}),
                              {kColumnID}, &values));
    EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
    values.clear();
  }
}

TEST_F(InMemoryStorageTest,
       DeleteUsingInvalidKeyRangeEndpointsReturnsInternalError) {
  absl::Time t0 = absl::Now();
  Key key({Int64(1)});

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, key, {}, {}));
  EXPECT_THAT(storage_.Delete(t0, kTableId0,
                              KeyRange::OpenClosed(Key({Int64(0)}), key)),
              googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(
      storage_.Delete(t0, kTableId0,
                      KeyRange::OpenOpen(Key({Int64(0)}), Key({Int64(2)}))),
      googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(storage_.Delete(t0, kTableId0,
                              KeyRange::ClosedClosed(Key({Int64(0)}), key)),
              googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
}

TEST_F(InMemoryStorageTest, DeleteUsingEmptyKeyRangeDeletesNothing) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }
  // Explicit Empty KeyRange.
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Empty()));

  // StartKey <= EndKey is considered an empty range.
  GOOGLESQL_EXPECT_OK(
      storage_.Delete(delete_ts, kTableId0,
                      KeyRange::ClosedOpen(Key({Int64(5)}), Key({Int64(0)}))));
  // Lookup keys in the range.
  for (int i = 0; i < 5; i++) {
    std::vector<googlesql::Value> values;
    GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(i)}),
                              {kColumnID}, &values));
    EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
    values.clear();
  }
  // Read
  GOOGLESQL_EXPECT_OK(
      storage_.Read(lookup_ts, kTableId0, KeyRange::All(), {kColumnID}, &itr_));
  for (int i = 0; i < 5; i++) {
    EXPECT_TRUE(itr_->Next());
    EXPECT_EQ(itr_->NumColumns(), 1);
    EXPECT_EQ(itr_->Key(), Key({Int64(i)}));
  }
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, DeleteUsingAllKeyRangeDeletesEverything) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::All()));
  // Lookup
  for (int i = 0; i < 5; i++) {
    std::vector<googlesql::Value> values;
    EXPECT_THAT(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(i)}),
                                {kColumnID}, &values),
                googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  }
  // Read
  GOOGLESQL_EXPECT_OK(
      storage_.Read(lookup_ts, kTableId0, KeyRange::All(), {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, DeleteUsingPrefixKeyRange) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({String("key"), Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0,
                            KeyRange::Prefix(Key({String("key")}))));
  // Lookup
  for (int i = 0; i < 5; i++) {
    std::vector<googlesql::Value> values;
    EXPECT_THAT(
        storage_.Lookup(lookup_ts, kTableId0, Key({String("key"), Int64(i)}),
                        {kColumnID}, &values),
        googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  }
  // Read
  GOOGLESQL_EXPECT_OK(storage_.Read(lookup_ts, kTableId0,
                          KeyRange::ClosedOpen(Key({String("key"), Int64(0)}),
                                               Key({String("key"), Int64(5)})),
                          {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupShouldReturnMostRecentColumnValues) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time write_after_delete_ts = delete_ts + absl::Seconds(1);
  absl::Time lookup_after_second_write_ts =
      write_after_delete_ts + absl::Seconds(1);
  Key key({Int64(1)});

  // Write key with column value = true.
  GOOGLESQL_EXPECT_OK(
      storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  // Delete key.
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Point(key)));
  // Write key without column value.
  GOOGLESQL_EXPECT_OK(storage_.Write(write_after_delete_ts, kTableId0, key, {}, {}));
  // Lookup should return empty column value.
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_after_second_write_ts, kTableId0, key,
                            {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(googlesql::Value()));
  // Read should return empty column values.
  GOOGLESQL_EXPECT_OK(storage_.Read(lookup_after_second_write_ts, kTableId0,
                          kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->NumColumns(), 1);
  EXPECT_FALSE(itr_->ColumnValue(0).is_valid());
  EXPECT_EQ(itr_->Key(), Key({Int64(1)}));
}

TEST_F(InMemoryStorageTest, LookupAtOrAfterDeleteTimestampReturnsInvalidValue) {
  Key key({Int64(1)});
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time after_delete_ts = delete_ts + absl::Seconds(1);

  GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, key, {kColumnID},
                           {String("value-10")}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Point(key)));

  // Lookup at delete timestamp.
  std::vector<googlesql::Value> values;
  EXPECT_THAT(storage_.Lookup(delete_ts, kTableId0, key, {kColumnID}, &values),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));

  // Lookup after delete timestamp.
  values.clear();
  EXPECT_THAT(
      storage_.Lookup(after_delete_ts, kTableId0, key, {kColumnID}, &values),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(InMemoryStorageTest, DeleteRemovesRowRegardlessOfReadTimestamp) {
  const auto now = absl::Now();
  const Key key({Int64(1)});
  GOOGLESQL_ASSERT_OK(storage_.Write(now, kTableId0, key, {kColumnID}, {Int64(1)}));
  GOOGLESQL_ASSERT_OK(storage_.Delete(now + absl::Seconds(1), kTableId0,
                                     KeyRange::Point(key)));
  std::vector<googlesql::Value> values;
  EXPECT_THAT(storage_.Lookup(now, kTableId0, key, {kColumnID}, &values),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(InMemoryStorageTest, MaterializedIteratorOwnsItsValues) {
  const auto now = absl::Now();
  const Key key({Int64(1)});
  GOOGLESQL_ASSERT_OK(storage_.Write(now, kTableId0, key, {kColumnID}, {Int64(1)}));
  GOOGLESQL_ASSERT_OK(storage_.Read(now, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  GOOGLESQL_ASSERT_OK(storage_.Delete(now, kTableId0, KeyRange::All()));
  ASSERT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->ColumnValue(0), Int64(1));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, ReadUsingKeyRangeAll) {
  absl::Time write_ts = absl::Now();
  absl::Time read_ts = write_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }

  // Read using KeyRange::All()
  GOOGLESQL_EXPECT_OK(
      storage_.Read(read_ts, kTableId0, KeyRange::All(), {kColumnID}, &itr_));
  for (int i = 0; i < 5; i++) {
    EXPECT_TRUE(itr_->Next());
    EXPECT_EQ(itr_->NumColumns(), 1);
    EXPECT_EQ(itr_->Key(), Key({Int64(i)}));
  }
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, ReadUsingPointKeyRange) {
  absl::Time write_ts = absl::Now();
  absl::Time read_ts = write_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }

  GOOGLESQL_EXPECT_OK(storage_.Read(read_ts, kTableId0, KeyRange::Point(Key({Int64(0)})),
                          {kColumnID}, &itr_));

  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->NumColumns(), 1);
  EXPECT_EQ(itr_->Key(), Key({Int64(0)}));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, ReadUsingPrefixKeyRange) {
  absl::Time write_ts = absl::Now();
  absl::Time read_ts = write_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({String("key"), Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }

  // Read
  GOOGLESQL_EXPECT_OK(storage_.Read(read_ts, kTableId0,
                          KeyRange::Prefix(Key({String("key")})), {kColumnID},
                          &itr_));
  for (int i = 0; i < 5; i++) {
    EXPECT_TRUE(itr_->Next());
    EXPECT_EQ(itr_->NumColumns(), 1);
    EXPECT_EQ(itr_->Key(), Key({String("key"), Int64(i)}));
  }
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest,
       ReadUsingInvalidKeyRangeEndpointsReturnsInternalError) {
  absl::Time t0 = absl::Now();
  Key key({Int64(1)});

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, key, {}, {}));

  EXPECT_THAT(
      storage_.Read(t0, kTableId0, KeyRange::OpenClosed(Key({Int64(0)}), key),
                    {}, &itr_),
      googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(
      storage_.Read(t0, kTableId0,
                    KeyRange::OpenOpen(Key({Int64(0)}), Key({Int64(2)})), {},
                    &itr_),
      googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(
      storage_.Read(t0, kTableId0, KeyRange::ClosedClosed(Key({Int64(0)}), key),
                    {}, &itr_),
      googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
}

TEST_F(InMemoryStorageTest, DroppedTablesAreRemovedAfterRetentionPeriod) {
  absl::Time t0 = absl::Now();

  // Write into 2 tables.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId1, Key({Int64(10)}), {kColumnID},
                           {String("value-10")}));

  // Drop the first table.
  storage_.MarkDroppedTable(t0, kTableId0);

  // Expire the table.
  storage_.CleanUpDeletedTables(t0 + absl::Hours(1) + absl::Seconds(1));

  // Lookup should return not found for first table.
  std::vector<googlesql::Value> values;
  EXPECT_THAT(
      storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(t0, kTableId1, Key({Int64(10)}), {kColumnID}, &values));
}

TEST_F(InMemoryStorageTest, DroppedColumnsAreRemovedAfterRetentionPeriod) {
  absl::Time t0 = absl::Now();

  // Write multiple rows to column.
  for (int i = 0; i < 10; i++) {
    GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(i)}), {kColumnID},
                             {String("value-1")}));
  }

  // Drop and expire the column.
  storage_.MarkDroppedColumn(t0, kTableId0, kColumnID);
  storage_.CleanUpDeletedColumns(t0 + absl::Hours(1) + absl::Seconds(1));

  // Lookup of column should return empty values in all rows.
  for (int i = 0; i < 10; i++) {
    std::vector<googlesql::Value> values;
    GOOGLESQL_EXPECT_OK(
        storage_.Lookup(t0, kTableId0, Key({Int64(i)}), {kColumnID}, &values));
    EXPECT_THAT(values, testing::ElementsAre(googlesql::Value()));
  }
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
