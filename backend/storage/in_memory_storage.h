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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_IN_MEMORY_STORAGE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_IN_MEMORY_STORAGE_H_

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/storage/iterator.h"
#include "backend/storage/storage.h"
#include "googlesql/public/value.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// InMemoryStorage holds only current values, with keys in sorted order.
// Active snapshots retain the original value of each row changed after they
// were created. Without snapshots, deletes physically remove rows and writes
// replace values directly. Timestamps cannot select historical versions.
//
// Lookup and Read return invalid googlesql::Value(s) for non-existent columns.
//
// This class is thread-safe.
class InMemoryStorage : public Storage {
 public:
  std::unique_ptr<Storage> CreateSnapshot() override ABSL_LOCKS_EXCLUDED(mu_);

  absl::Status Lookup(absl::Time timestamp, const TableID& table_id,
                      const Key& key, const std::vector<ColumnID>& column_ids,
                      std::vector<googlesql::Value>* values) const override
      ABSL_LOCKS_EXCLUDED(mu_);

  absl::Status Read(absl::Time timestamp, const TableID& table_id,
                    const KeyRange& key_range,
                    const std::vector<ColumnID>& column_ids,
                    std::unique_ptr<StorageIterator>* itr) const override
      ABSL_LOCKS_EXCLUDED(mu_);

  absl::Status Write(absl::Time timestamp, const TableID& table_id,
                     const Key& key, const std::vector<ColumnID>& column_ids,
                     const std::vector<googlesql::Value>& values) override
      ABSL_LOCKS_EXCLUDED(mu_);

  absl::Status Delete(absl::Time timestamp, const TableID& table_id,
                      const KeyRange& key_range) override
      ABSL_LOCKS_EXCLUDED(mu_);

  void SetVersionRetentionPeriod(
      absl::Duration version_retention_period) override;

  void CleanUpDeletedTables(absl::Time timestamp) override
      ABSL_LOCKS_EXCLUDED(mu_);

  void CleanUpDeletedColumns(absl::Time timestamp) override
      ABSL_LOCKS_EXCLUDED(mu_);

  void MarkDroppedTable(absl::Time timestamp, TableID dropped_table_id) override
      ABSL_LOCKS_EXCLUDED(mu_);

  void MarkDroppedColumn(absl::Time timestamp, TableID dropped_table_id,
                         ColumnID dropped_column_id) override
      ABSL_LOCKS_EXCLUDED(mu_);

 private:
  using Row = absl::flat_hash_map<ColumnID, googlesql::Value>;
  using Table = std::map<Key, Row>;
  using Tables = absl::flat_hash_map<TableID, Table>;

  struct SnapshotState {
    // A missing optional masks a row inserted after the snapshot was created.
    using Changes = std::map<Key, std::optional<Row>>;
    absl::flat_hash_map<TableID, Changes> before_images;
  };
  class Snapshot;

  void PreserveRow(const TableID& table_id, const Key& key, const Row* row)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  absl::Status LookupSnapshot(const SnapshotState* snapshot,
                              absl::Time timestamp, const TableID& table_id,
                              const Key& key,
                              const std::vector<ColumnID>& column_ids,
                              std::vector<googlesql::Value>* values) const
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status ReadSnapshot(const SnapshotState* snapshot, absl::Time timestamp,
                            const TableID& table_id, const KeyRange& key_range,
                            const std::vector<ColumnID>& column_ids,
                            std::unique_ptr<StorageIterator>* itr) const
      ABSL_LOCKS_EXCLUDED(mu_);

  // Returns an invalid value for an absent column.
  googlesql::Value GetCellValue(const Row& row, const ColumnID& column_id) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  mutable absl::Mutex mu_;
  Tables tables_ ABSL_GUARDED_BY(mu_);
  // Snapshot contents are also guarded by mu_. Expired entries are pruned on
  // registration and writes, so read-only workloads cannot grow this list.
  std::vector<std::weak_ptr<SnapshotState>> snapshots_ ABSL_GUARDED_BY(mu_);

  // Tracks when tables were dropped so that we can clean up the data.
  std::map<absl::Time, TableID> dropped_tables_ ABSL_GUARDED_BY(mu_);

  // Tracks when columns were dropped so that we can clean up the data.
  std::map<absl::Time, std::pair<TableID, ColumnID>> dropped_columns_
      ABSL_GUARDED_BY(mu_);

  mutable absl::Mutex version_retention_period_mu_ ABSL_ACQUIRED_AFTER(mu_);
  absl::Duration version_retention_period_
      ABSL_GUARDED_BY(version_retention_period_mu_) = absl::Hours(1);
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_IN_MEMORY_STORAGE_H_
