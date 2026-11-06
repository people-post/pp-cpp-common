#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pp {

/**
 * Replace a whole file atomically: a temp file of its own in the same directory
 * (owner-only from creation on POSIX: files may hold secrets), fsync, rename
 * over the target, fsync the directory. Missing parent directories are created.
 * Readers see the old content or the new, never a partial write, and two
 * writers of one path at once each succeed (the last rename wins).
 *
 * Windows does not replace a file another handle has open (a reader, a virus
 * scanner looking at a file just written): the rename is tried again for about
 * 0.13 s before Write fails.
 */
class AtomicFileWrite {
public:
  /**
   * Durable: the file and its directory are synced before Write returns, so a
   * power cut leaves the new content or the old. None: replaced atomically, not
   * synced — for caches and data that can be fetched or made again, where an
   * fsync per write costs more than it keeps.
   */
  enum class Sync { Durable, None };

  static Roe<void> Write(const std::string &path, std::string_view data,
                         Sync sync = Sync::Durable);
  static Roe<void> Write(const std::string &path,
                         const std::vector<uint8_t> &data,
                         Sync sync = Sync::Durable);
  /** `prefix` then `data` as one file (a header before a large payload, without
   * a joined copy of it). */
  static Roe<void> Write(const std::string &path, std::string_view prefix,
                         const std::vector<uint8_t> &data,
                         Sync sync = Sync::Durable);
};

} // namespace pp
