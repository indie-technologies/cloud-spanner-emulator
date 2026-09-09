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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_VERSIONED_CATALOG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_VERSIONED_CATALOG_H_

#include <memory>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/schema/catalog/schema.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Owns the current committed schema. Historical timestamp lookup is unsupported.
// Shared snapshots keep schemas alive only while a transaction or admin request
// still references them; migration count no longer determines retained memory.
class VersionedCatalog {
 public:
  // The default constructor creates an empty schema in the catalog and assigns
  // absl::InfinitePast() as its creation timestamp.
  VersionedCatalog();

  // The single-argument constructor is used when a database is created with an
  // initial schema specified. The initial_schema is added to the catalog and
  // absl::InfinitePast() is assigned as its creation timestamp.
  explicit VersionedCatalog(std::unique_ptr<const Schema> initial_schema);

  // The raw pointer is valid while the caller excludes schema changes.
  const Schema* GetLatestSchema() const ABSL_LOCKS_EXCLUDED(mu_);

  // Use a shared snapshot when the schema may outlive database ownership,
  // including admin requests that run concurrently with schema updates.
  std::shared_ptr<const Schema> GetLatestSchemaSnapshot() const
      ABSL_LOCKS_EXCLUDED(mu_);

  // Adds a schema at a given timestamp. Returns an error if creation_time is
  // the same or prior to the current schema timestamp. In this case, the
  // current schema is unchanged. Unreferenced previous schemas are reclaimed.
  absl::Status AddSchema(absl::Time creation_time,
                         std::unique_ptr<const Schema> schema)
      ABSL_LOCKS_EXCLUDED(mu_);

  absl::Duration version_retention_period() const {
    absl::MutexLock lock(&mu_);
    return version_retention_period_;
  }

 private:
  mutable absl::Mutex mu_;
  std::shared_ptr<const Schema> latest_schema_ ABSL_GUARDED_BY(mu_);
  absl::Time creation_time_ ABSL_GUARDED_BY(mu_) = absl::InfinitePast();

  // Compatibility option used for cleanup of dropped storage objects.
  absl::Duration version_retention_period_ ABSL_GUARDED_BY(mu_) =
      absl::Hours(1);
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_VERSIONED_CATALOG_H_
