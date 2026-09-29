#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <regex>
#include <unordered_set>

using pp::util::GenerateUuid;

TEST(UtilitiesTest, GenerateUuidMatchesRfc4122Version4) {
  // 8-4-4-4-12 hex, version nibble 4, variant nibble 8/9/a/b.
  static const std::regex kUuidV4(
      "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  for (int i = 0; i < 100; ++i) {
    const std::string uuid = GenerateUuid();
    EXPECT_TRUE(std::regex_match(uuid, kUuidV4)) << uuid;
  }
}

TEST(UtilitiesTest, GenerateUuidIsNotRepeatedAcrossCalls) {
  // Predictable/seeded output (the pre-fix mt19937_64 seeded from a single
  // 32-bit random_device draw) would make collisions far more likely.
  std::unordered_set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    EXPECT_TRUE(seen.insert(GenerateUuid()).second);
  }
}
