#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pp {

/**
 * Replace a whole file atomically: a temp file in the same directory (owner-only from creation on
 * POSIX: files may hold secrets), fsync, rename over the target, fsync the directory. Missing parent
 * directories are created. Readers see the old content or the new, never a partial write.
 */
class AtomicFileWrite {
public:
  static Roe<void> Write(const std::string& path, std::string_view data);
  static Roe<void> Write(const std::string& path, const std::vector<uint8_t>& data);
};

} // namespace pp
