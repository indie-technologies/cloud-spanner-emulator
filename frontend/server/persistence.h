// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0.
#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_SERVER_PERSISTENCE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_SERVER_PERSISTENCE_H_
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "frontend/server/environment.h"

namespace google::spanner::emulator::frontend {
// Owns the file lock and checkpoint thread. The environment outlives this
// object. Open is startup-only, before listening; Checkpoint may run during
// requests.
class Persistence {
 public:
  static absl::StatusOr<std::unique_ptr<Persistence>> Open(
      ServerEnv* env, const std::string& path);
  ~Persistence();
  void Start(absl::Duration interval);
  void Stop();
  // Returns true only when a new checkpoint was published. Serialized with
  // other checkpoints; dirty checks never mark writes made during a save clean.
  absl::StatusOr<bool> Checkpoint();

 private:
  explicit Persistence(ServerEnv* env, std::string path)
      : env_(env), path_(std::move(path)) {}
  struct Version {
    std::vector<uint64_t> collections;
    std::vector<backend::Database::PersistenceVersion> databases;
    bool operator==(const Version&) const = default;
  };
  struct Resources {
    persistence::Snapshot metadata;
    std::vector<std::shared_ptr<Database>> databases;
    Version version;
  };
  Resources CaptureResources();
  absl::Status Restore(const persistence::Snapshot& snapshot);
  ServerEnv* env_;
  std::string path_;
  int lock_fd_ = -1;
  std::mutex checkpoint_mutex_;
  std::optional<Version> saved_version_;
  std::mutex worker_mutex_;
  std::condition_variable wake_;
  bool stopped_ = false;
  std::thread worker_;
};
}  // namespace google::spanner::emulator::frontend
#endif
