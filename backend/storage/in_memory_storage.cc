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

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "backend/storage/in_memory_iterator.h"
#include "common/errors.h"
#include "googlesql/public/value.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

class InMemoryStorage::Snapshot : public Storage {
 public:
  Snapshot(InMemoryStorage* storage, std::shared_ptr<SnapshotState> state)
      : storage_(storage), state_(std::move(state)) {}

  std::unique_ptr<Storage> CreateSnapshot() override {
    return std::make_unique<Snapshot>(storage_, state_);
  }
  absl::Status Lookup(absl::Time timestamp, const TableID& table_id,
                      const Key& key, const std::vector<ColumnID>& columns,
                      std::vector<googlesql::Value>* values) const override {
    return storage_->LookupSnapshot(state_.get(), timestamp, table_id, key,
                                    columns, values);
  }
  absl::Status Read(absl::Time timestamp, const TableID& table_id,
                    const KeyRange& range, const std::vector<ColumnID>& columns,
                    std::unique_ptr<StorageIterator>* itr) const override {
    return storage_->ReadSnapshot(state_.get(), timestamp, table_id, range,
                                  columns, itr);
  }
  absl::Status Write(absl::Time, const TableID&, const Key&,
                     const std::vector<ColumnID>&,
                     const std::vector<googlesql::Value>&) override {
    return absl::FailedPreconditionError("Cannot write to a storage snapshot");
  }
  absl::Status Delete(absl::Time, const TableID&, const KeyRange&) override {
    return absl::FailedPreconditionError(
        "Cannot delete from a storage snapshot");
  }
  void SetVersionRetentionPeriod(absl::Duration) override {}
  void CleanUpDeletedTables(absl::Time) override {}
  void CleanUpDeletedColumns(absl::Time) override {}
  void MarkDroppedTable(absl::Time, TableID) override {}
  void MarkDroppedColumn(absl::Time, TableID, ColumnID) override {}

 private:
  InMemoryStorage* storage_;
  std::shared_ptr<SnapshotState> state_;
};

std::unique_ptr<Storage> InMemoryStorage::CreateSnapshot() {
  absl::MutexLock lock(mu_);
  snapshots_.erase(std::remove_if(snapshots_.begin(), snapshots_.end(),
                                  [](const auto& s) { return s.expired(); }),
                   snapshots_.end());
  auto state = std::make_shared<SnapshotState>();
  snapshots_.push_back(state);
  return std::make_unique<Snapshot>(this, std::move(state));
}

void InMemoryStorage::PreserveRow(const TableID& table_id, const Key& key,
                                  const Row* row) {
  for (auto it = snapshots_.begin(); it != snapshots_.end();) {
    if (auto snapshot = it->lock()) {
      // Only the first change matters, even if this row is updated many times
      // while a reader is alive. Avoid copying values on subsequent writes.
      auto [before, inserted] =
          snapshot->before_images[table_id].try_emplace(key);
      if (inserted && row != nullptr) before->second.emplace(*row);
      ++it;
    } else {
      it = snapshots_.erase(it);
    }
  }
}

googlesql::Value InMemoryStorage::GetCellValue(
    const Row& row, const ColumnID& column_id) const {
  auto cell = row.find(column_id);
  return cell == row.end() ? googlesql::Value() : cell->second;
}

absl::Status InMemoryStorage::Lookup(
    absl::Time timestamp, const TableID& table_id, const Key& key,
    const std::vector<ColumnID>& column_ids,
    std::vector<googlesql::Value>* values) const {
  return LookupSnapshot(nullptr, timestamp, table_id, key, column_ids, values);
}

absl::Status InMemoryStorage::LookupSnapshot(
    const SnapshotState* snapshot, absl::Time timestamp,
    const TableID& table_id, const Key& key,
    const std::vector<ColumnID>& column_ids,
    std::vector<googlesql::Value>* values) const {
  absl::MutexLock lock(mu_);

  // Validate the request.
  if (!column_ids.empty() && values == nullptr) {
    return error::Internal(
        "InMemoryStorage::Lookup was passed a nullptr for "
        "values, but had non-empty column_ids.");
  }
  if (values != nullptr) {
    values->clear();
  }

  const Row* row = nullptr;
  auto table_itr = tables_.find(table_id);
  if (table_itr != tables_.end()) {
    auto row_itr = table_itr->second.find(key);
    if (row_itr != table_itr->second.end()) row = &row_itr->second;
  }
  if (snapshot != nullptr) {
    auto table = snapshot->before_images.find(table_id);
    if (table != snapshot->before_images.end()) {
      auto before = table->second.find(key);
      if (before != table->second.end()) {
        row = before->second ? &*before->second : nullptr;
      }
    }
  }
  if (row == nullptr) {
    return absl::Status(
        absl::StatusCode::kNotFound,
        absl::StrCat("Key: ", key.DebugString(), " not found for table: ",
                     table_id, " at timestamp: ", absl::FormatTime(timestamp)));
  }

  // For request without columns, return ok since the key exist.
  if (column_ids.empty()) {
    return absl::OkStatus();
  }

  // Fetch the current value of each column.
  values->reserve(column_ids.size());
  for (const auto& column_id : column_ids) {
    values->emplace_back(GetCellValue(*row, column_id));
  }

  return absl::OkStatus();
}

absl::Status InMemoryStorage::Read(
    absl::Time timestamp, const TableID& table_id, const KeyRange& key_range,
    const std::vector<ColumnID>& column_ids,
    std::unique_ptr<StorageIterator>* itr) const {
  return ReadSnapshot(nullptr, timestamp, table_id, key_range, column_ids, itr);
}

absl::Status InMemoryStorage::ReadSnapshot(
    const SnapshotState* snapshot, absl::Time timestamp,
    const TableID& table_id, const KeyRange& key_range,
    const std::vector<ColumnID>& column_ids,
    std::unique_ptr<StorageIterator>* itr) const {
  absl::MutexLock lock(mu_);

  // Validate the request.
  if (!key_range.IsClosedOpen()) {
    return error::Internal(
        absl::StrCat("InMemoryStorage::Read should be called "
                     "with ClosedOpen key range, found: ",
                     key_range.DebugString()));
  }

  std::vector<FixedRowStorageIterator::Row> rows;
  // Return an empty iterator for empty key_range.
  if (key_range.start_key() >= key_range.limit_key()) {
    *itr = std::make_unique<FixedRowStorageIterator>();
    return absl::OkStatus();
  }

  const Table empty_table;
  const SnapshotState::Changes empty_changes;
  auto table_itr = tables_.find(table_id);
  const Table& table =
      table_itr == tables_.end() ? empty_table : table_itr->second;
  const SnapshotState::Changes* changes = &empty_changes;
  if (snapshot != nullptr) {
    auto before = snapshot->before_images.find(table_id);
    if (before != snapshot->before_images.end()) changes = &before->second;
  }
  auto append = [&](const Key& key, const Row& row) {
    std::vector<googlesql::Value> values;
    values.reserve(column_ids.size());
    for (const ColumnID& column_id : column_ids) {
      values.emplace_back(GetCellValue(row, column_id));
    }
    rows.emplace_back(key, std::move(values));
  };
  auto current = table.lower_bound(key_range.start_key());
  auto current_end = table.lower_bound(key_range.limit_key());
  auto before = changes->lower_bound(key_range.start_key());
  auto before_end = changes->lower_bound(key_range.limit_key());
  while (current != current_end || before != before_end) {
    if (before != before_end &&
        (current == current_end || !(current->first < before->first))) {
      if (before->second) append(before->first, *before->second);
      if (current != current_end && current->first == before->first) ++current;
      ++before;
    } else {
      append(current->first, current->second);
      ++current;
    }
  }
  *itr = std::make_unique<FixedRowStorageIterator>(std::move(rows));
  return absl::OkStatus();
}

absl::Status InMemoryStorage::Write(
    absl::Time timestamp, const TableID& table_id, const Key& key,
    const std::vector<ColumnID>& column_ids,
    const std::vector<googlesql::Value>& values) {
  absl::MutexLock lock(mu_);

  // Add the table if it does not exist.
  Table& table = tables_[table_id];

  // An empty row still represents an existing key.
  auto [existing, inserted] = table.try_emplace(key);
  Row& row = existing->second;
  PreserveRow(table_id, key, inserted ? nullptr : &row);
  for (int i = 0; i < column_ids.size(); ++i) {
    row[column_ids[i]] = values[i];
  }

  return absl::OkStatus();
}

absl::Status InMemoryStorage::Delete(absl::Time timestamp,
                                     const TableID& table_id,
                                     const KeyRange& key_range) {
  absl::MutexLock lock(mu_);

  if (!key_range.IsClosedOpen()) {
    return error::Internal(
        absl::StrCat("InMemoryStorage::Delete should be called "
                     "with ClosedOpen key range, found: ",
                     key_range.DebugString()));
  }
  if (key_range.start_key() >= key_range.limit_key()) {
    return absl::OkStatus();
  }

  // Lookup for given table.
  auto table_itr = tables_.find(table_id);
  if (table_itr == tables_.end()) {
    return absl::OkStatus();
  }
  Table& table = table_itr->second;

  // Lookup keys from the given key range.
  auto row_start_itr = table.lower_bound(key_range.start_key());
  if (row_start_itr == table.end()) {
    return absl::OkStatus();
  }
  auto row_end_itr = table.lower_bound(key_range.limit_key());

  if (!snapshots_.empty()) {
    for (auto it = row_start_itr; it != row_end_itr; ++it) {
      PreserveRow(table_id, it->first, &it->second);
    }
  }

  table.erase(row_start_itr, row_end_itr);
  return absl::OkStatus();
}

void InMemoryStorage::SetVersionRetentionPeriod(
    const absl::Duration version_retention_period) {
  absl::MutexLock lock(version_retention_period_mu_);
  version_retention_period_ = version_retention_period;
}

void InMemoryStorage::CleanUpDeletedTables(absl::Time timestamp) {
  absl::MutexLock lock(mu_);
  absl::MutexLock version_retention_period_lock(version_retention_period_mu_);
  absl::Time expiration_time = timestamp - version_retention_period_;

  // Remove expired dropped tables.
  for (auto it = dropped_tables_.begin();
       it != dropped_tables_.upper_bound(expiration_time);) {
    auto table = tables_.find(it->second);
    if (table != tables_.end() && !snapshots_.empty()) {
      for (const auto& [key, row] : table->second) {
        PreserveRow(table->first, key, &row);
      }
    }
    tables_.erase(it->second);
    it = dropped_tables_.erase(it);
  }
}

void InMemoryStorage::CleanUpDeletedColumns(absl::Time timestamp) {
  absl::MutexLock lock(mu_);
  absl::MutexLock version_retention_period_lock(version_retention_period_mu_);
  absl::Time expiration_time = timestamp - version_retention_period_;

  // Remove expired dropped columns.
  for (auto it = dropped_columns_.begin();
       it != dropped_columns_.upper_bound(expiration_time);) {
    auto [table_id, column_id] = it->second;
    for (auto& [key, row] : tables_[table_id]) {
      if (!row.contains(column_id)) continue;
      PreserveRow(table_id, key, &row);
      row.erase(column_id);
    }
    it = dropped_columns_.erase(it);
  }
}

void InMemoryStorage::MarkDroppedTable(absl::Time timestamp,
                                       TableID dropped_table_id) {
  absl::MutexLock lock(mu_);
  dropped_tables_[timestamp] = dropped_table_id;
}

void InMemoryStorage::MarkDroppedColumn(absl::Time timestamp,
                                        TableID dropped_table_id,
                                        ColumnID dropped_column_id) {
  absl::MutexLock lock(mu_);
  dropped_columns_[timestamp] =
      std::make_pair(dropped_table_id, dropped_column_id);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
