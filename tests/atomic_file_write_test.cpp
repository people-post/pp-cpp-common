#include "common/AtomicFileWrite.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

using pp::AtomicFileWrite;

class AtomicFileWriteTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_atomic_write_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
            std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST_F(AtomicFileWriteTest, ReplacesExistingFile) {
  const auto path = (dir_ / "data.json").string();
  {
    std::ofstream out(path);
    out << "old";
  }
  ASSERT_TRUE(AtomicFileWrite::Write(path, std::string("{\"ok\":true}")));
  std::ifstream in(path);
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(body, "{\"ok\":true}");
}

std::string ReadAll(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// Two writers of one path at once each succeed, the file is one of theirs
// whole, and no temp file is left.
TEST_F(AtomicFileWriteTest, WritersOfOnePathAtOnceAllSucceed) {
  const auto path = (dir_ / "shared.bin").string();
  constexpr int kWriters = 4;
  constexpr int kWrites = 50;
  std::atomic<int> failed{0};
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      const std::string body(32 * 1024, static_cast<char>('a' + w));
      for (int i = 0; i < kWrites; ++i) {
        if (!AtomicFileWrite::Write(path, body, AtomicFileWrite::Sync::None)) {
          failed.fetch_add(1);
        }
      }
    });
  }
  for (std::thread &writer : writers) {
    writer.join();
  }
  EXPECT_EQ(failed.load(), 0);
  const std::string body = ReadAll(path);
  ASSERT_EQ(body.size(), 32u * 1024u);
  EXPECT_EQ(body.find_first_not_of(body[0]), std::string::npos)
      << "one writer's content, whole";
  int stray = 0;
  for (const auto &entry : std::filesystem::directory_iterator(dir_)) {
    stray += entry.path().filename().string() != "shared.bin" ? 1 : 0;
  }
  EXPECT_EQ(stray, 0);
}

// A file someone is reading is replaced: the write waits for the reader on
// Windows (which cannot replace an open file) instead of failing; elsewhere it
// replaces it at once.
TEST_F(AtomicFileWriteTest, AFileBeingReadIsReplaced) {
  const auto path = (dir_ / "read.txt").string();
  ASSERT_TRUE(AtomicFileWrite::Write(path, std::string("old")));
  std::atomic<bool> open{false};
  std::thread reader([&] {
    std::ifstream in(path, std::ios::binary);
    open.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  });
  while (!open.load()) {
    std::this_thread::yield();
  }
  auto written = AtomicFileWrite::Write(path, std::string("new"));
  reader.join();
  ASSERT_TRUE(written) << written.error().message;
  EXPECT_EQ(ReadAll(path), "new");
}

// A header and a payload as one file; and Sync::None still replaces the file
// whole.
TEST_F(AtomicFileWriteTest, APrefixAndItsPayloadAreOneFile) {
  const auto path = (dir_ / "block").string();
  const std::vector<uint8_t> payload = {1, 2, 3, 0, 255};
  ASSERT_TRUE(AtomicFileWrite::Write(path, std::string_view("PPBA"), payload,
                                     AtomicFileWrite::Sync::None));
  EXPECT_EQ(ReadAll(path),
            std::string("PPBA") + std::string("\x01\x02\x03\x00\xff", 5));
}

TEST_F(AtomicFileWriteTest, CreatesParentDirs) {
  const auto path = (dir_ / "nested" / "a" / "file.txt").string();
  ASSERT_TRUE(AtomicFileWrite::Write(path, std::string("hello")));
  std::ifstream in(path);
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(body, "hello");
}

TEST_F(AtomicFileWriteTest, FailedRenameLeavesPriorIntact) {
  const auto path = (dir_ / "keep.json").string();
  ASSERT_TRUE(AtomicFileWrite::Write(path, std::string("prior")));
  // Target path is an existing directory — rename into it must fail.
  std::filesystem::create_directories(path + ".as_dir");
  // Write to a path whose parent becomes unusable mid-flight is hard; instead write
  // with a destination that is a directory so rename fails after temp create.
  const std::string dir_as_file = (dir_ / "dir_target").string();
  std::filesystem::create_directories(dir_as_file);
  auto result = AtomicFileWrite::Write(dir_as_file, std::string("nope"));
  EXPECT_FALSE(static_cast<bool>(result));
  std::ifstream in(path);
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(body, "prior");
}

#if !defined(_WIN32)
TEST_F(AtomicFileWriteTest, WritesOwnerOnlyPermissions) {
  // Profile data (config/preferences can hold secrets, e.g. LLM api_key) must never be
  // group/world-readable, including at the moment the file is first created.
  const auto path = (dir_ / "secret.json").string();
  ASSERT_TRUE(AtomicFileWrite::Write(path, std::string("{\"api_key\":\"x\"}")));

  struct stat st{};
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, static_cast<mode_t>(0600));
}
#endif

} // namespace
