#!/usr/bin/env bash
# Build, test and package a native Linux release. Run at the repository root.
set -euo pipefail
cd "$(dirname "$0")/../.."
case "$(uname -m)" in x86_64) arch=amd64 ;; aarch64) arch=arm64 ;; *) echo 'Unsupported architecture' >&2; exit 1 ;; esac
[[ "$(uname -s)" == Linux ]] || { echo 'Release builds require Linux' >&2; exit 1; }
revision=$(git rev-parse HEAD)
[[ -z "$(git status --porcelain --untracked-files=no)" ]] || { echo 'Release source has uncommitted changes' >&2; exit 1; }
out="${RELEASE_OUTPUT_DIR:-$PWD/dist}"
mkdir -p "$out"
out=$(cd "$out" && pwd)
BAZEL="${BAZEL:-bazel}"
flags=(-c opt --jobs="${BUILD_JOBS:-8}" --local_resources="memory=${BUILD_MEMORY_MB:-24000}" --features=-layering_check --lockfile_mode=off)
"$BAZEL" build "${flags[@]}" //binaries:emulator_main //binaries:gateway_main
"$BAZEL" test "${flags[@]}" --test_output=errors \
  //backend/database:database_test //backend/database:snapshot_test \
  //backend/database/change_stream:change_stream_partition_churner_test \
  //backend/storage:in_memory_storage_test \
  //backend/transaction:read_only_transaction_test //backend/transaction:read_write_transaction_test \
  //frontend/server:persistence_test //tests/persistence:process_test
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp bazel-bin/binaries/emulator_main bazel-bin/binaries/gateway_main_/gateway_main LICENSE "$stage/"
external=$("$BAZEL" info output_base)/external
python3 build/release/package.py "$stage" "$external" "$revision" "$arch"
cp build/release/Dockerfile "$stage/Dockerfile"
archive="cloud-spanner-emulator-linux-$arch.tar.gz"
tar -czf "$out/$archive" -C "$stage" .
(cd "$out" && sha256sum "$archive" > "$archive.sha256")
echo "Release archive: $out/$archive ($revision)"
