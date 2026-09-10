// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0.
#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_PERSISTENCE_FILES_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_PERSISTENCE_FILES_H_
#include <string>

#include "absl/status/status.h"
#include "backend/persistence/snapshot.pb.h"
namespace google::spanner::emulator::persistence {
inline constexpr uint32_t kFormatVersion = 1;
// NOT_FOUND is returned only for a missing destination, never for corruption.
absl::Status ReadSnapshotFile(const std::string& path, Snapshot* snapshot);
absl::Status WriteSnapshotFile(const std::string& path,
                               const Snapshot& snapshot);
}  // namespace google::spanner::emulator::persistence
#endif
