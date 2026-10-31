#include "common/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <thread>

// B79: the UI mailbox used to be created lazily by whichever thread first touched it. On a fresh
// install a worker's PostUI (directory refresh reply) ran before the main thread's InitializeUI,
// so CurrentlyOnUI() was false on the main thread forever and RefreshPendingRing re-posted
// itself until the run loop never presented a frame. Initialize() must bind the mailbox to the
// calling (main) thread before any worker can post.
TEST(UiMailboxThreadTest, InitializeBindsUiMailboxToCallingThreadBeforeWorkersPost) {
  pp::AppRuntime::ShutdownUI(); // drop a mailbox left over from an earlier test
  pp::AppRuntime::Initialize();

  std::thread other([]() { pp::AppRuntime::PostUI([]() {}); });
  other.join();

  EXPECT_TRUE(pp::AppRuntime::CurrentlyOnUI());

  pp::AppRuntime::ShutdownUI();
  pp::AppRuntime::Shutdown();
}
