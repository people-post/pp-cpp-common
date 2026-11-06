#include "common/AtomicFileWrite.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pp {
namespace {

/** What a temp file is made of, in order. */
struct Part {
  const char *data = nullptr;
  size_t size = 0;
};

#if defined(_WIN32)

/** A refused replace is tried again this many times, waiting 2, 4 … 64 ms
 * (about 0.13 s in all). */
constexpr int kRenameRetries = 6;

/** The replace was refused for a handle someone else has open — one that closes
 * soon, unlike a missing directory. */
bool Transient(const DWORD error) {
  return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
         error == ERROR_LOCK_VIOLATION;
}

uint64_t Pid() { return static_cast<uint64_t>(_getpid()); }

Roe<void> FsyncFile(const std::filesystem::path& path) {
  const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error("Failed to open for fsync: " + path.string());
  }
  if (!FlushFileBuffers(handle)) {
    CloseHandle(handle);
    return Error("fsync failed: " + path.string());
  }
  CloseHandle(handle);
  return {};
}

void FsyncDirectory(const std::filesystem::path& dir) {
  const HANDLE handle = CreateFileW(dir.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }
  (void)FlushFileBuffers(handle);
  CloseHandle(handle);
}

Roe<void> RenameOver(const std::filesystem::path& tmp_path, const std::filesystem::path& final_path) {
  for (int attempt = 0;; ++attempt) {
    if (MoveFileExW(tmp_path.c_str(), final_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return {};
    }
    const DWORD error = GetLastError();
    if (!Transient(error) || attempt >= kRenameRetries) {
      return Error(
          "Failed to rename atomic write into place: " + final_path.string() +
          " (error " + std::to_string(error) + ")");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2 << attempt));
  }
}

/** Windows ACLs are not modeled: the temp file keeps the directory's inherited ACL. */
Roe<void> WriteTemp(const std::filesystem::path &tmp_path,
                    std::initializer_list<Part> parts) {
  std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Error("Failed to open temp file for atomic write: " + tmp_path.string());
  }
  for (const Part &part : parts) {
    out.write(part.data, static_cast<std::streamsize>(part.size));
  }
  out.flush();
  if (!out) {
    return Error("Failed to write temp file: " + tmp_path.string());
  }
  return {};
}

#else

uint64_t Pid() { return static_cast<uint64_t>(::getpid()); }

Roe<void> FsyncFile(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Error("Failed to open for fsync: " + path.string());
  }
  if (::fsync(fd) != 0) {
    const int err = errno;
    ::close(fd);
    return Error(std::string("fsync failed: ") + std::strerror(err));
  }
  ::close(fd);
  return {};
}

void FsyncDirectory(const std::filesystem::path& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return;
  }
  (void)::fsync(fd);
  ::close(fd);
}

Roe<void> RenameOver(const std::filesystem::path& tmp_path, const std::filesystem::path& final_path) {
  std::error_code ec;
  std::filesystem::rename(tmp_path, final_path, ec);
  if (ec) {
    return Error("Failed to rename atomic write into place: " + final_path.string() + " (" + ec.message() + ")");
  }
  return {};
}

/**
 * Owner-only (0600) from the moment the file exists — config / preferences can hold secrets — rather
 * than widening the window with default permissions until a later chmod.
 */
Roe<void> WriteTemp(const std::filesystem::path &tmp_path,
                    std::initializer_list<Part> parts) {
  const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return Error(std::string("Failed to open temp file for atomic write: ") + std::strerror(errno));
  }
  for (const Part &part : parts) {
    size_t written = 0;
    while (written < part.size) {
      const ssize_t n = ::write(fd, part.data + written, part.size - written);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        const int err = errno;
        ::close(fd);
        return Error(std::string("Failed to write temp file: ") +
                     std::strerror(err));
      }
      written += static_cast<size_t>(n);
    }
  }
  if (::close(fd) != 0) {
    return Error(std::string("Failed to close temp file: ") + std::strerror(errno));
  }
  return {};
}

#endif

std::filesystem::path MakeTempPath(const std::filesystem::path& final_path) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::filesystem::path tmp = final_path;
  tmp += ".tmp.";
  tmp += std::to_string(Pid());
  tmp += ".";
  tmp += std::to_string(gen());
  return tmp;
}

Roe<void> WriteBytes(const std::string &path, std::initializer_list<Part> parts,
                     const AtomicFileWrite::Sync sync) {
  const bool durable = sync == AtomicFileWrite::Sync::Durable;
  const std::filesystem::path final_path(path);
  std::error_code ec;
  std::filesystem::create_directories(final_path.parent_path(), ec);

  const std::filesystem::path tmp_path = MakeTempPath(final_path);
  const auto fail = [&tmp_path](Error error) -> Roe<void> {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return error;
  };
  if (auto written = WriteTemp(tmp_path, parts); !written) {
    return fail(written.error());
  }
  if (durable) {
    if (auto synced = FsyncFile(tmp_path); !synced) {
      return fail(synced.error());
    }
  }
  if (auto renamed = RenameOver(tmp_path, final_path); !renamed) {
    return fail(renamed.error());
  }
  if (durable) {
    FsyncDirectory(final_path.parent_path());
  }
  return {};
}

} // namespace

Roe<void> AtomicFileWrite::Write(const std::string &path, std::string_view data,
                                 const Sync sync) {
  return WriteBytes(path, {Part{data.data(), data.size()}}, sync);
}

Roe<void> AtomicFileWrite::Write(const std::string &path,
                                 const std::vector<uint8_t> &data,
                                 const Sync sync) {
  return WriteBytes(
      path, {Part{reinterpret_cast<const char *>(data.data()), data.size()}},
      sync);
}

Roe<void> AtomicFileWrite::Write(const std::string &path,
                                 std::string_view prefix,
                                 const std::vector<uint8_t> &data,
                                 const Sync sync) {
  return WriteBytes(
      path,
      {Part{prefix.data(), prefix.size()},
       Part{reinterpret_cast<const char *>(data.data()), data.size()}},
      sync);
}

} // namespace pp
