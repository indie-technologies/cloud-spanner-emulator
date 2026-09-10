// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0.
#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_PERSISTENCE_VALUES_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_PERSISTENCE_VALUES_H_
#include "absl/status/statusor.h"
#include "backend/persistence/snapshot.pb.h"
#include "googlesql/public/value.h"
namespace google::spanner::emulator::persistence {
absl::Status SerializeValue(const googlesql::Value& value, Value* out);
absl::StatusOr<googlesql::Value> DeserializeValue(const Value& value,
                                                  const googlesql::Type* type);
}  // namespace google::spanner::emulator::persistence
#endif
