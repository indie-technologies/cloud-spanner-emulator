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
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "backend/access/read.h"
#include "backend/common/ids.h"
#include "backend/common/rows.h"
#include "backend/datamodel/key_set.h"
#include "backend/locking/manager.h"
#include "backend/storage/in_memory_iterator.h"
#include "backend/storage/storage.h"
#include "backend/transaction/options.h"
#include "backend/transaction/resolve.h"
#include "backend/transaction/row_cursor.h"
#include "common/clock.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

ReadOnlyTransaction::ReadOnlyTransaction(
    const ReadOnlyOptions& options, TransactionID transaction_id, Clock* clock,
    Storage* storage, LockManager* lock_manager,
    const VersionedCatalog* const versioned_catalog)
    : options_(options),
      id_(transaction_id),
      clock_(clock),
      base_storage_(storage),
      lock_manager_(lock_manager),
      versioned_catalog_(versioned_catalog) {
  absl::MutexLock lock(mu_);
  if (options.bound != TimestampBound::kStrongRead) {
    status_ = absl::UnimplementedError(
        "Only strong reads are supported by this emulator");
    return;
  }
  absl::MutexLock snapshot_lock(lock_manager_->snapshot_mutex());
  read_timestamp_ = clock_->Now();
  base_storage_->CleanUpDeletedTables(read_timestamp_);
  storage_snapshot_ = base_storage_->CreateSnapshot();
  schema_snapshot_ = versioned_catalog_->GetLatestSchemaSnapshot();
  schema_ = schema_snapshot_.get();
}

ReadOnlyTransaction::~ReadOnlyTransaction() { Close(); }

absl::Status ReadOnlyTransaction::status() const {
  absl::MutexLock lock(mu_);
  return status_;
}

void ReadOnlyTransaction::Close() {
  absl::MutexLock lock(mu_);
  if (status_.ok()) status_ = error::TransactionClosed(id_);
  if (active_requests_ == 0) storage_snapshot_.reset();
}

absl::Status ReadOnlyTransaction::GuardedCall(
    const std::function<absl::Status()>& fn) {
  absl::ReaderMutexLock schema_lock(lock_manager_->schema_mutex());
  {
    absl::MutexLock lock(mu_);
    GOOGLESQL_RETURN_IF_ERROR(status_);
    ++active_requests_;
  }
  const absl::Status result = fn();
  {
    absl::MutexLock lock(mu_);
    --active_requests_;
    if (active_requests_ == 0 && !status_.ok()) storage_snapshot_.reset();
  }
  return result;
}

absl::Status ReadOnlyTransaction::Read(const ReadArg& read_arg,
                                       std::unique_ptr<RowCursor>* cursor) {
  absl::MutexLock lock(mu_);
  GOOGLESQL_RETURN_IF_ERROR(status_);
  GOOGLESQL_ASSIGN_OR_RETURN(const ResolvedReadArg resolved_read_arg,
                             ResolveReadArg(read_arg, schema()));

  std::vector<std::unique_ptr<StorageIterator>> iterators;
  for (const auto& key_range : resolved_read_arg.key_ranges) {
    std::unique_ptr<StorageIterator> itr;
    GOOGLESQL_RETURN_IF_ERROR(storage_snapshot_->Read(
        read_timestamp_, resolved_read_arg.table->id(), key_range,
        GetColumnIDs(resolved_read_arg.columns), &itr));
    iterators.push_back(std::move(itr));
  }
  *cursor = std::make_unique<StorageIteratorRowCursor>(
      std::move(iterators), resolved_read_arg.columns);
  return absl::OkStatus();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
