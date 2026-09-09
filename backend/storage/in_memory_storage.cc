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

#include <memory>
#include <utility>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "backend/storage/in_memory_iterator.h"
#include "common/errors.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

googlesql::Value InMemoryStorage::GetCellValue(
    const Row& row, const ColumnID& column_id) const {
  auto cell = row.find(column_id);
  return cell == row.end() ? googlesql::Value() : cell->second;
}

absl::Status InMemoryStorage::Lookup(
    absl::Time timestamp, const TableID& table_id, const Key& key,
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

  // Lookup for given table.
  auto table_itr = tables_.find(table_id);
  if (table_itr == tables_.end()) {
    return absl::Status(
        absl::StatusCode::kNotFound,
        absl::StrCat("Key: ", key.DebugString(), " not found for table: ",
                     table_id, " at timestamp: ", absl::FormatTime(timestamp)));
  }
  const Table& table = table_itr->second;

  // Lookup for given key.
  auto row_itr = table.find(key);
  if (row_itr == table.end()) {
    return absl::Status(
        absl::StatusCode::kNotFound,
        absl::StrCat("Key: ", key.DebugString(), " not found for table: ",
                     table_id, " at timestamp: ", absl::FormatTime(timestamp)));
  }
  const Row& row = row_itr->second;

  // For request without columns, return ok since the key exist.
  if (column_ids.empty()) {
    return absl::OkStatus();
  }

  // Fetch the current value of each column.
  values->reserve(column_ids.size());
  for (const auto& column_id : column_ids) {
    values->emplace_back(GetCellValue(row, column_id));
  }

  return absl::OkStatus();
}

absl::Status InMemoryStorage::Read(
    absl::Time timestamp, const TableID& table_id, const KeyRange& key_range,
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

  // Lookup for given table.
  auto table_itr = tables_.find(table_id);
  if (table_itr == tables_.end()) {
    *itr = std::make_unique<FixedRowStorageIterator>();
    return absl::OkStatus();
  }
  const Table& table = table_itr->second;

  // Lookup keys from the given key range.
  auto row_start_itr = table.lower_bound(key_range.start_key());
  auto row_end_itr = table.lower_bound(key_range.limit_key());
  for (auto itr = row_start_itr; itr != row_end_itr; ++itr) {
    const InMemoryStorage::Row& row = itr->second;

    std::vector<googlesql::Value> values;
    values.reserve(column_ids.size());
    for (const ColumnID& column_id : column_ids) {
      values.emplace_back(GetCellValue(row, column_id));
    }
    rows.emplace_back(itr->first, std::move(values));
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
  Row& row = table[key];
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
    for (auto& [_, row] : tables_[table_id]) {
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
