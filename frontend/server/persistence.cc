// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0.
#include "frontend/server/persistence.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <set>

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "backend/persistence/files.h"
#include "frontend/common/uris.h"
#include "googlesql/base/status_macros.h"

namespace google::spanner::emulator::frontend {
absl::StatusOr<std::unique_ptr<Persistence>> Persistence::Open(
    ServerEnv* env, const std::string& path) {
  if (!env->instance_manager()->Capture().entries.empty() ||
      !env->database_manager()->Capture().entries.empty() ||
      !env->instance_partition_manager()->Capture().entries.empty()) {
    return absl::FailedPreconditionError(
        "Persistence must be opened on an empty, unpublished environment");
  }
  auto state = std::unique_ptr<Persistence>(new Persistence(env, path));
  // Prevent two emulators racing to replace the same destination. A separate
  // inode is needed because checkpoints atomically replace the snapshot inode.
  state->lock_fd_ =
      open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (state->lock_fd_ < 0 || flock(state->lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Cannot lock state file '", path, "': ", std::strerror(errno)));
  }
  persistence::Snapshot snapshot;
  auto status = persistence::ReadSnapshotFile(path, &snapshot);
  if (absl::IsNotFound(status)) {
    state->saved_version_ = state->CaptureResources().version;
    return state;
  }
  GOOGLESQL_RETURN_IF_ERROR(status);
  GOOGLESQL_RETURN_IF_ERROR(state->Restore(snapshot));
  state->saved_version_ = state->CaptureResources().version;
  return state;
}
Persistence::~Persistence() {
  Stop();
  if (lock_fd_ >= 0) close(lock_fd_);
}
void Persistence::Start(absl::Duration interval) {
  worker_ = std::thread([this, interval] {
    std::unique_lock lock(worker_mutex_);
    while (!wake_.wait_for(lock, absl::ToChronoNanoseconds(interval),
                           [this] { return stopped_; })) {
      lock.unlock();
      auto result = Checkpoint();
      if (!result.ok())
        ABSL_LOG(ERROR) << "Checkpoint failed for " << path_ << ": "
                        << result.status();
      lock.lock();
    }
  });
}
void Persistence::Stop() {
  {
    std::lock_guard lock(worker_mutex_);
    stopped_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}
Persistence::Resources Persistence::CaptureResources() {
  auto instances = env_->instance_manager()->Capture();
  auto partitions = env_->instance_partition_manager()->Capture();
  auto databases = env_->database_manager()->Capture();
  Resources result;
  result.metadata.set_format_version(persistence::kFormatVersion);
  result.version.collections = {instances.revision, partitions.revision,
                                databases.revision};
  std::set<std::string> parents;
  for (const auto& instance : instances.entries) {
    instance->ToProto(result.metadata.add_instances());
    parents.insert(instance->instance_uri());
  }
  for (const auto& partition : partitions.entries) {
    const auto& uri = partition->partition_uri();
    if (parents.contains(uri.substr(0, uri.rfind("/instancePartitions/")))) {
      partition->ToProto(result.metadata.add_partitions());
    }
  }
  for (const auto& database : databases.entries) {
    const auto& uri = database->database_uri();
    // Admin deletes cascade across managers. Pin the resource handles, and
    // never publish a database without its parent. Collection revisions ensure
    // a concurrent create/drop is revisited at the next checkpoint.
    if (parents.contains(uri.substr(0, uri.rfind("/databases/")))) {
      result.databases.push_back(database);
      result.version.databases.push_back(
          database->backend()->GetPersistenceVersion());
    }
  }
  return result;
}
absl::StatusOr<bool> Persistence::Checkpoint() {
  std::lock_guard lock(checkpoint_mutex_);
  auto resources = CaptureResources();
  if (saved_version_ && *saved_version_ == resources.version) return false;
  auto started = absl::Now();
  for (const auto& db : resources.databases) {
    auto snapshot = db->backend()->CaptureSnapshot();
    auto* out = resources.metadata.add_databases();
    out->set_uri(db->database_uri());
    GOOGLESQL_RETURN_IF_ERROR(snapshot.Serialize(out));
  }
  GOOGLESQL_RETURN_IF_ERROR(
      persistence::WriteSnapshotFile(path_, resources.metadata));
  // Use the version from BEFORE capture, never the current version: changes
  // during capture/serialization remain dirty, even if a save included some.
  saved_version_ = std::move(resources.version);
  ABSL_LOG(INFO) << "Checkpoint saved " << resources.metadata.ByteSizeLong()
                 << " bytes to " << path_ << " in " << absl::Now() - started;
  return true;
}
absl::Status Persistence::Restore(const persistence::Snapshot& snapshot) {
  if (!env_->instance_manager()->Capture().entries.empty() ||
      !env_->database_manager()->Capture().entries.empty()) {
    return absl::FailedPreconditionError(
        "Restore requires an empty, unpublished environment");
  }
  std::set<std::string> instances;
  for (const auto& instance : snapshot.instances()) {
    absl::string_view project, id;
    GOOGLESQL_RETURN_IF_ERROR(ParseInstanceUri(instance.name(), &project, &id));
    if (!instances.insert(instance.name()).second)
      return absl::DataLossError("Duplicate snapshot instance");
    GOOGLESQL_RETURN_IF_ERROR(env_->instance_manager()->Restore(instance));
  }
  for (const auto& partition : snapshot.partitions()) {
    absl::string_view project, instance, id;
    GOOGLESQL_RETURN_IF_ERROR(
        ParseInstancePartitionUri(partition.name(), &project, &instance, &id));
    if (!instances.contains(MakeInstanceUri(project, instance)))
      return absl::DataLossError("Orphan snapshot partition");
    GOOGLESQL_RETURN_IF_ERROR(
        env_->instance_partition_manager()->Restore(partition));
  }
  auto started = absl::Now();
  for (const auto& database : snapshot.databases()) {
    absl::string_view project, instance, id;
    GOOGLESQL_RETURN_IF_ERROR(
        ParseDatabaseUri(database.uri(), &project, &instance, &id));
    if (!instances.contains(MakeInstanceUri(project, instance)))
      return absl::DataLossError("Orphan snapshot database");
    auto restored = backend::Database::Restore(env_->clock(), id, database);
    if (!restored.ok()) {
      return absl::Status(
          restored.status().code(),
          absl::StrCat("Cannot restore database ", database.uri(), ": ",
                       restored.status().message()));
    }
    auto backend = std::move(*restored);
    GOOGLESQL_RETURN_IF_ERROR(env_->database_manager()->AddRestoredDatabase(
        std::make_shared<Database>(database.uri(), std::move(backend),
                                   env_->clock()->Now())));
  }
  ABSL_LOG(INFO) << "Restored " << snapshot.databases_size()
                 << " databases from " << path_ << " in "
                 << absl::Now() - started;
  return absl::OkStatus();
}
}  // namespace google::spanner::emulator::frontend
