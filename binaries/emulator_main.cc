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

#include <algorithm>
#include <memory>
#include <csignal>
#include <pthread.h>

#include "absl/flags/flag.h"
#include "absl/time/time.h"

#include "absl/flags/parse.h"
#include "googlesql/base/logging.h"
#include "absl/strings/str_cat.h"
#include "common/config.h"
#include "frontend/server/server.h"

ABSL_FLAG(std::string, state_file, "", "Optional development snapshot file; missing files start empty.");
ABSL_FLAG(absl::Duration, checkpoint_interval, absl::Seconds(60), "Checkpoint interval when state_file is set.");

using Server = ::google::spanner::emulator::frontend::Server;

int main(int argc, char** argv) {
  // Block before creating worker threads. sigwait handles shutdown in ordinary
  // code, where draining RPCs, joining threads and file I/O are safe.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) return EXIT_FAILURE;
  // Start the emulator gRPC server.
  absl::ParseCommandLine(argc, argv);
  Server::Options options;
  options.server_address = google::spanner::emulator::config::grpc_host_port();
  options.state_file = absl::GetFlag(FLAGS_state_file);
  options.checkpoint_interval = absl::GetFlag(FLAGS_checkpoint_interval);
  std::unique_ptr<Server> server = Server::Create(options);
  if (!server) {
    ABSL_LOG(ERROR) << "Failed to start gRPC server.";
    return EXIT_FAILURE;
  }

  ABSL_LOG(INFO) << "Cloud Spanner Emulator running.";
  ABSL_LOG(INFO) << "Server address: "
            << absl::StrCat(server->host(), ":", server->port());

  int received;
  if (sigwait(&signals, &received) != 0) return EXIT_FAILURE;
  auto status = server->Shutdown();
  if (!status.ok()) {
    ABSL_LOG(ERROR) << "Final checkpoint failed: " << status;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
