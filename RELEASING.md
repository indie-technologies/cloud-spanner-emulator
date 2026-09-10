# Releasing the Found fork

The maintained branch is `codex/development-persistence`. It includes the query
and migration optimizations, concurrent strong readers with one writer, and
optional native development persistence. Found consumes immutable release
artifacts, not a checkout of a moving branch.

## Build and release

1. Review and land changes on the maintained branch. Open a pull request to run
   `.github/workflows/release.yml` before tagging.
2. Create a new `v<upstream>-found<N>` tag at the reviewed commit and push it.
   For example, the first native persistence release is `v1.5.57-found2`.
3. The release workflow builds optimized Linux amd64 binaries, runs eight
   storage/database/transaction/persistence test targets, and publishes the
   archive and SHA-256 file to GitHub Releases. Never move a published tag or
   replace its assets; use a new release number for fixes.
4. Check the run completed successfully and retain its source revision and
   archive checksum. `build-info.json` inside the archive records both binary
   hashes, architecture and exact source revision. Dependency licenses and the
   runtime Dockerfile are included.

A clean Linux checkout can reproduce the process with `build/release/build.sh`.
Use GCC 13, Java 21 and the Bazel version in `.bazelversion`. Set `BUILD_JOBS` and
`BUILD_MEMORY_MB` for the host (the workflow uses 16 jobs / 80 GB; a 24 GB Coder
workspace should use 2 jobs / 14 GB). `BAZEL` may name a wrapper that selects a
persistent Bazel output directory. `RELEASE_OUTPUT_DIR` defaults to `dist`.
The script checks that tracked source is clean before building.

## Promote to Found

Found's `development/spanner/emulator/VERSION` pins the release, full revision
and archive checksum. Its installer verifies these before selecting the native
binaries for Coder. Found's `spanner-dev: verify` workflow exercises startup,
one-time import of legacy dumps, and a full Found schema restore.

Found's `spanner-dev: build` workflow, dispatched on trusted `main`, verifies and
tests the pinned artifact, packages it with the included runtime Dockerfile,
smoke-tests the container and publishes to Artifact Registry and GHCR. Found
pins the resulting image digests in Compose and CI. See Found's
`development/spanner/emulator/README.md` for the promotion sequence and the
first-release bootstrap procedure when new workflow code is still in a PR.

Only Linux amd64 is published by the current workflow. Apple Silicon uses the
amd64 image through Docker's emulation; native ARM releases can be added as a
separate build matrix once validated. See [PERSISTENCE.md](PERSISTENCE.md) for
snapshot compatibility, shutdown behavior and measured costs.
