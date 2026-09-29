#include "common/Utilities.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <stdexcept>

#if defined(_WIN32)
// CRT/Windows shim — see docs/architecture/PLATFORM_CODE.md (allowlisted in common/).
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <cstdlib> // arc4random_buf
#elif defined(__linux__) || defined(__ANDROID__)
#include <cerrno>
#include <sys/random.h>
#else
#include <cstdio>
#endif

namespace pp::util {

namespace {

// Fills `bytes` with OS-CSPRNG output. GenerateUuid's callers use the result
// as a nonce/identifier, so a seeded PRNG (predictable given the seed) is not
// acceptable here.
void FillRandomBytes(uint8_t *bytes, size_t count) {
#if defined(_WIN32)
  const NTSTATUS status = BCryptGenRandom(
      nullptr, bytes, static_cast<ULONG>(count),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status != 0 /* STATUS_SUCCESS */) {
    throw std::runtime_error("BCryptGenRandom failed");
  }
#elif defined(__APPLE__)
  arc4random_buf(bytes, count);
#elif defined(__linux__) || defined(__ANDROID__)
  size_t filled = 0;
  while (filled < count) {
    const ssize_t n = getrandom(bytes + filled, count - filled, 0);
    if (n > 0) {
      filled += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    throw std::runtime_error("getrandom failed");
  }
#else
  std::FILE *f = std::fopen("/dev/urandom", "rb");
  if (!f) {
    throw std::runtime_error("failed to open /dev/urandom");
  }
  const size_t n = std::fread(bytes, 1, count, f);
  std::fclose(f);
  if (n != count) {
    throw std::runtime_error("short read from /dev/urandom");
  }
#endif
}

} // namespace

std::string GenerateUuid() {
  std::array<uint8_t, 16> bytes{};
  FillRandomBytes(bytes.data(), bytes.size());
  // RFC 4122 version 4 (random), variant 1 (10xx).
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

  static const char *kHex = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out.push_back('-');
    }
    out.push_back(kHex[(bytes[i] >> 4) & 0x0F]);
    out.push_back(kHex[bytes[i] & 0x0F]);
  }
  return out;
}

int64_t NowUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Trim(const std::string& text) {
  const auto start = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (start >= end) {
    return {};
  }
  return std::string(start, end);
}

std::string ToLowerAscii(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

} // namespace pp::util
