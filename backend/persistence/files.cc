// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0.
#include "backend/persistence/files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <vector>

#include "absl/crc/crc32c.h"
#include "absl/strings/str_cat.h"
#include "googlesql/base/status_macros.h"

namespace google::spanner::emulator::persistence {
namespace {
constexpr char kMagic[] = "SPANSNAP";
constexpr size_t kHeaderSize =
    24;  // magic, version(u32), length(u64), CRC32C(u32)
absl::Status IoError(const std::string& path) {
  return absl::UnknownError(absl::StrCat("Snapshot I/O error for '", path,
                                         "': ", std::strerror(errno)));
}
void AppendInteger(uint64_t value, int bytes, std::string* out) {
  for (int i = 0; i < bytes; ++i) {
    out->push_back(value & 255);
    value >>= 8;
  }
}
uint64_t Integer(const char* data, int bytes) {
  uint64_t result = 0;
  for (int i = 0; i < bytes; ++i)
    result |= uint64_t(static_cast<unsigned char>(data[i])) << (i * 8);
  return result;
}
absl::Status Transfer(int fd, char* data, size_t length, bool writing,
                      const std::string& path) {
  while (length) {
    ssize_t count = writing ? write(fd, data, length) : read(fd, data, length);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return IoError(path);
    if (count == 0)
      return absl::DataLossError(absl::StrCat("Truncated snapshot: ", path));
    data += count;
    length -= count;
  }
  return absl::OkStatus();
}
struct File {
  int fd;
  ~File() {
    if (fd >= 0) close(fd);
  }
};
}  // namespace
absl::Status ReadSnapshotFile(const std::string& path, Snapshot* snapshot) {
  File file{open(path.c_str(), O_RDONLY | O_CLOEXEC)};
  if (file.fd < 0) {
    if (errno == ENOENT)
      return absl::NotFoundError(
          absl::StrCat("Snapshot does not exist: ", path));
    return IoError(path);
  }
  struct stat info;
  if (fstat(file.fd, &info) != 0) return IoError(path);
  if (!S_ISREG(info.st_mode))
    return absl::InvalidArgumentError("State file must be a regular file");
  if (info.st_size < kHeaderSize)
    return absl::DataLossError("Truncated snapshot header");
  std::string header(kHeaderSize, '\0');
  GOOGLESQL_RETURN_IF_ERROR(
      Transfer(file.fd, header.data(), header.size(), false, path));
  if (header.compare(0, 8, kMagic) != 0)
    return absl::DataLossError("Invalid snapshot magic (expected SPANSNAP)");
  if (Integer(header.data() + 8, 4) != kFormatVersion) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Incompatible snapshot version ", Integer(header.data() + 8, 4),
        "; this emulator supports version ", kFormatVersion));
  }
  auto length = Integer(header.data() + 12, 8);
  if (length != info.st_size - kHeaderSize ||
      length > std::numeric_limits<int>::max()) {
    return absl::DataLossError(
        "Invalid snapshot length (maximum payload is 2 GiB)");
  }
  std::string payload(length, '\0');
  GOOGLESQL_RETURN_IF_ERROR(
      Transfer(file.fd, payload.data(), payload.size(), false, path));
  if (uint32_t(absl::ComputeCrc32c(payload)) !=
      Integer(header.data() + 20, 4)) {
    return absl::DataLossError("Snapshot checksum mismatch");
  }
  if (!snapshot->ParseFromString(payload))
    return absl::DataLossError("Invalid snapshot protobuf");
  if (snapshot->format_version() != kFormatVersion)
    return absl::FailedPreconditionError(
        "Incompatible snapshot protobuf version");
  return absl::OkStatus();
}
absl::Status WriteSnapshotFile(const std::string& path,
                               const Snapshot& snapshot) {
  if (snapshot.format_version() != kFormatVersion)
    return absl::InvalidArgumentError("Unsupported snapshot version");
  if (snapshot.ByteSizeLong() > std::numeric_limits<int>::max())
    return absl::ResourceExhaustedError("Snapshot payload exceeds 2 GiB");
  std::string payload;
  if (!snapshot.SerializeToString(&payload))
    return absl::InternalError("Could not serialize snapshot");
  std::string header(kMagic, 8);
  AppendInteger(kFormatVersion, 4, &header);
  AppendInteger(payload.size(), 8, &header);
  AppendInteger(uint32_t(absl::ComputeCrc32c(payload)), 4, &header);
  std::string temporary = path + ".tmp.XXXXXX";
  File file{mkstemp(temporary.data())};  // same directory/filesystem, mode 0600
  if (file.fd < 0) return IoError(temporary);
  auto result =
      Transfer(file.fd, header.data(), header.size(), true, temporary);
  if (result.ok())
    result = Transfer(file.fd, payload.data(), payload.size(), true, temporary);
  if (result.ok() && fsync(file.fd) != 0) result = IoError(temporary);
  int fd = file.fd;
  file.fd = -1;
  if (close(fd) != 0 && result.ok()) result = IoError(temporary);
  if (result.ok() && rename(temporary.c_str(), path.c_str()) != 0)
    result = IoError(path);
  if (!result.ok()) unlink(temporary.c_str());
  return result;
}
}  // namespace google::spanner::emulator::persistence
