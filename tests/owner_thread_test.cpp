#include "common/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Owner threads are serialized mailboxes behind the teardown gate — Dedicated (own OS thread, the app)
// or Manual (drained by the caller, tests). The app registers them; the runtime names none.

namespace {

using namespace std::chrono_literals;
using pp::AppRuntime;
using pp::AppRuntimeConfig;
using pp::OwnerThreadId;
using pp::OwnerThreadMode;

constexpr OwnerThreadId kOwner{0};
constexpr OwnerThreadId kSecondOwner{3};

class OwnerThreadTest : public ::testing::Test {
protected:
  void Start(OwnerThreadMode mode) {
    AppRuntime::InitializeUI();
    AppRuntimeConfig config;
    config.owners = {{kOwner, "pp-test-owner"}, {kSecondOwner, "pp-test-other"}};
    config.owner_threads = mode;
    config.name_thread = [this](const std::string& name) {
      std::lock_guard lock(names_mu_);
      names_.push_back(name);
    };
    AppRuntime::Initialize(config);
  }
  void TearDown() override {
    AppRuntime::ReopenAfterTeardown();
    AppRuntime::Shutdown();
  }

  template <typename Pred>
  static bool WaitFor(Pred&& done, std::chrono::milliseconds budget = 2s) {
    const auto until = std::chrono::steady_clock::now() + budget;
    while (!done()) {
      if (std::chrono::steady_clock::now() >= until) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  std::mutex names_mu_;
  std::vector<std::string> names_;
};

TEST_F(OwnerThreadTest, DedicatedRunsTasksInOrderOnItsOwnNamedThread) {
  Start(OwnerThreadMode::Dedicated);
  std::mutex mu;
  std::vector<int> order;
  std::atomic<bool> on_owner{true};
  std::thread::id owner_thread;
  for (int i = 0; i < 20; ++i) {
    AppRuntime::PostTo(kOwner, [&, i] {
      std::lock_guard lock(mu);
      order.push_back(i);
      owner_thread = std::this_thread::get_id();
      if (!AppRuntime::CurrentlyOn(kOwner)) {
        on_owner = false;
      }
    });
  }
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard lock(mu);
    return order.size() == 20;
  }));
  std::lock_guard lock(mu);
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(order[i], i);
  }
  EXPECT_TRUE(on_owner.load());
  EXPECT_NE(owner_thread, std::this_thread::get_id());
  EXPECT_FALSE(AppRuntime::CurrentlyOn(kOwner)) << "the test thread is not the owner";
  std::lock_guard names_lock(names_mu_);
  EXPECT_NE(std::find(names_.begin(), names_.end(), "pp-test-owner"), names_.end());
}

// A test lane (MakeLaneForTesting: a fake mesh's io lane) is drained with the owners, so chains
// across them run to the end.
TEST_F(OwnerThreadTest, ManualRunsOnlyWhenDrainedAndFollowsChainsThroughATestLane) {
  Start(OwnerThreadMode::Manual);
  ASSERT_TRUE(AppRuntime::OwnerThreadsManual());
  auto lane = AppRuntime::MakeLaneForTesting();
  std::vector<std::string> trace;
  AppRuntime::PostTo(kOwner, [&] {
    trace.push_back(AppRuntime::CurrentlyOn(kOwner) ? "media" : "media-off-owner");
    lane->Post([&] {
      trace.push_back(lane->RunsTasksOnCurrentThread() ? "lane" : "lane-off");
      AppRuntime::PostTo(kOwner, [&] { trace.push_back("media-reply"); });
    });
  });
  std::this_thread::sleep_for(20ms);
  EXPECT_TRUE(trace.empty()) << "manual owners and lanes have no thread of their own";
  EXPECT_EQ(AppRuntime::RunAllOwnerTasks(), 3u);
  EXPECT_EQ(trace, (std::vector<std::string>{"media", "lane", "media-reply"}));
  EXPECT_FALSE(AppRuntime::CurrentlyOn(kOwner));
  bool waited = false;
  lane->RunAndWait([&] { waited = lane->RunsTasksOnCurrentThread(); });
  EXPECT_TRUE(waited) << "RunAndWait drains a manual lane on the caller";
}

TEST_F(OwnerThreadTest, ScheduleOnPostsOntoTheOwnerAfterTheDelay) {
  Start(OwnerThreadMode::Dedicated);
  std::atomic<bool> on_owner{false};
  const auto posted = std::chrono::steady_clock::now();
  std::atomic<long long> waited_ms{0};
  ASSERT_NE(AppRuntime::ScheduleOn(kOwner, 30ms, [&] {
              waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - posted)
                              .count();
              on_owner = AppRuntime::CurrentlyOn(kOwner);
            }),
            0u);
  ASSERT_TRUE(WaitFor([&] { return on_owner.load(); }));
  EXPECT_GE(waited_ms.load(), 25);
}

TEST_F(OwnerThreadTest, QuiesceWaitsForRunningOwnerWorkThenDropsNewPosts) {
  Start(OwnerThreadMode::Dedicated);
  std::atomic<bool> finished{false};
  std::atomic<bool> started{false};
  AppRuntime::PostTo(kOwner, [&] {
    started = true;
    std::this_thread::sleep_for(50ms);
    finished = true;
  });
  ASSERT_TRUE(WaitFor([&] { return started.load(); }));
  EXPECT_TRUE(AppRuntime::QuiesceForTeardown(2s));
  EXPECT_TRUE(finished.load()) << "quiesce returned while an owner task was still running";
  std::atomic<bool> late{false};
  AppRuntime::PostTo(kOwner, [&] { late = true; });
  std::this_thread::sleep_for(30ms);
  EXPECT_FALSE(late.load()) << "posts after quiesce must no-op";
}

TEST_F(OwnerThreadTest, QuiescePumpsManualOwners) {
  Start(OwnerThreadMode::Manual);
  std::atomic<bool> ran{false};
  AppRuntime::PostTo(kOwner, [&] { ran = true; });
  EXPECT_TRUE(AppRuntime::QuiesceForTeardown(2s));
  EXPECT_TRUE(ran.load());
}

TEST_F(OwnerThreadTest, DrainWorkersThenUISitsBehindOwnerQueues) {
  Start(OwnerThreadMode::Dedicated);
  std::atomic<bool> owner_done{false};
  AppRuntime::PostTo(kOwner, [&] {
    std::this_thread::sleep_for(40ms);
    owner_done = true;
  });
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(2s));
  EXPECT_TRUE(owner_done.load());
}

// A task a stopped owner drops unrun must still settle the teardown gate: before, the next quiesce
// in the process waited out its whole budget for a task that never came.
TEST_F(OwnerThreadTest, DroppedPostsDoNotStallTheNextQuiesce) {
  Start(OwnerThreadMode::Manual);
  AppRuntime::PostTo(kOwner, [] {});
  AppRuntime::Shutdown();  // owner stops with the task still queued
  AppRuntime::Initialize();
  const auto began = std::chrono::steady_clock::now();
  EXPECT_TRUE(AppRuntime::QuiesceForTeardown(1s));
  EXPECT_LT(std::chrono::steady_clock::now() - began, 500ms);
}

TEST_F(OwnerThreadTest, PostsBeforeInitializeOrAfterShutdownAreDropped) {
  Start(OwnerThreadMode::Manual);
  AppRuntime::Shutdown();
  bool ran = false;
  AppRuntime::PostTo(kOwner, [&] { ran = true; });
  EXPECT_EQ(AppRuntime::RunAllOwnerTasks(), 0u);
  EXPECT_FALSE(ran);
  AppRuntime::Initialize();
}

// RunAndWait (t2b): a component's lifecycle edge driven from another thread runs on the owner and
// the caller waits; if the gate drops the post, the caller stands in for the owner.
TEST_F(OwnerThreadTest, RunAndWaitRunsOnTheDedicatedOwnerAndWaits) {
  Start(OwnerThreadMode::Dedicated);
  std::atomic<bool> first{false};
  AppRuntime::PostTo(kOwner, [&] {
    std::this_thread::sleep_for(20ms);
    first = true;
  });
  bool on_owner = false;
  bool after_first = false;
  AppRuntime::RunAndWait(kOwner, [&] {
    on_owner = AppRuntime::CurrentlyOn(kOwner);
    after_first = first.load();
  });
  EXPECT_TRUE(on_owner);
  EXPECT_TRUE(after_first) << "queued behind earlier owner work";
  EXPECT_FALSE(AppRuntime::CurrentlyOn(kOwner));
}

TEST_F(OwnerThreadTest, RunAndWaitDrainsAManualOwner) {
  Start(OwnerThreadMode::Manual);
  bool ran = false;
  AppRuntime::RunAndWait(kOwner, [&] {
    ran = AppRuntime::CurrentlyOn(kOwner);
  });
  EXPECT_TRUE(ran);
}

TEST_F(OwnerThreadTest, RunAndWaitStandsInForTheOwnerWhenThePostIsDropped) {
  Start(OwnerThreadMode::Dedicated);
  ASSERT_TRUE(AppRuntime::QuiesceForTeardown(1s));
  int runs = 0;
  bool stand_in = false;
  AppRuntime::RunAndWait(kOwner, [&] {
    ++runs;
    stand_in = AppRuntime::CurrentlyOn(kOwner);
    PP_ASSERT_ON_OWNER(kOwner);
  });
  EXPECT_EQ(runs, 1);
  EXPECT_TRUE(stand_in);
  EXPECT_FALSE(AppRuntime::CurrentlyOn(kOwner)) << "only for the task's duration";
  AppRuntime::ReopenAfterTeardown();
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(1s));
  EXPECT_EQ(runs, 1);
}

TEST_F(OwnerThreadTest, RunAndWaitWithoutAnOwnerRunsInline) {
  Start(OwnerThreadMode::Manual);
  AppRuntime::Shutdown();
  bool stand_in = false;
  AppRuntime::RunAndWait(kOwner,
                         [&] { stand_in = AppRuntime::CurrentlyOn(kOwner); });
  EXPECT_TRUE(stand_in);
  AppRuntime::Initialize();
}

#ifndef NDEBUG
TEST_F(OwnerThreadTest, AffinityAssertAbortsOffTheOwner) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        Start(OwnerThreadMode::Dedicated);
        PP_ASSERT_ON_OWNER(kOwner);
      },
      "affinity violated");
}

TEST_F(OwnerThreadTest, AffinityAssertPassesOnTheOwner) {
  Start(OwnerThreadMode::Manual);
  bool reached = false;
  AppRuntime::PostTo(kOwner, [&] {
    PP_ASSERT_ON_OWNER(kOwner);
    reached = true;
  });
  AppRuntime::RunAllOwnerTasks();
  EXPECT_TRUE(reached);
}
#endif

// The app registers its owners: only those exist, under their names, and a drain passes through each.
TEST_F(OwnerThreadTest, OnlyRegisteredOwnersExistAndDrainsPassThroughEach) {
  Start(OwnerThreadMode::Dedicated);
  EXPECT_TRUE(AppRuntime::HasOwner(kOwner));
  EXPECT_TRUE(AppRuntime::HasOwner(kSecondOwner));
  EXPECT_FALSE(AppRuntime::HasOwner(OwnerThreadId{1}));
  EXPECT_FALSE(AppRuntime::HasOwner(OwnerThreadId{200})) << "an id past kMaxOwnerThreads is no owner";
  EXPECT_EQ(AppRuntime::OwnerName(kSecondOwner), "pp-test-other");
  EXPECT_EQ(AppRuntime::OwnerName(OwnerThreadId{1}), "");

  bool ran_unregistered = false;
  AppRuntime::PostTo(OwnerThreadId{1}, [&] { ran_unregistered = true; });
  std::atomic<bool> second_done{false};
  AppRuntime::PostTo(kSecondOwner, [&] {
    std::this_thread::sleep_for(40ms);
    second_done = true;
  });
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(2s));
  EXPECT_TRUE(second_done.load()) << "the drain waits behind every registered owner";
  EXPECT_FALSE(ran_unregistered);
}

} // namespace
