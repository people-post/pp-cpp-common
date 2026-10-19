#include "common/Lifetime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>

using pp::Lifetime;
using namespace std::chrono_literals;

TEST(LifetimeTest, BoundWorkRunsUntilInvalidatedAndNewWorkRunsAfter) {
  Lifetime lifetime;
  int ran = 0;
  auto before = lifetime.Bind([&ran]() { ++ran; });
  before();
  EXPECT_EQ(ran, 1);
  lifetime.Invalidate();
  before();
  EXPECT_EQ(ran, 1) << "bound before: a no-op now";
  auto after = lifetime.Bind([&ran](int n) { ran += n; });
  after(5);
  EXPECT_EQ(ran, 6) << "bound after: runs (an owner that aborted mid-life goes on)";
}

TEST(LifetimeTest, WorkBoundBeforeTheOwnerWentAwayDoesNothing) {
  int ran = 0;
  std::function<void()> late;
  {
    Lifetime lifetime;
    late = lifetime.Bind([&ran]() { ++ran; });
  }
  late();
  EXPECT_EQ(ran, 0);
}

TEST(LifetimeTest, PostHandsOverABoundTask) {
  Lifetime lifetime;
  std::function<void()> queued;
  int ran = 0;
  lifetime.Post([&queued](std::function<void()> task) { queued = std::move(task); }, [&ran]() { ++ran; });
  lifetime.Invalidate();
  queued();
  EXPECT_EQ(ran, 0);
}

TEST(LifetimeTest, HandChecksSeeTheGeneration) {
  Lifetime lifetime;
  const auto token = lifetime.token();
  const uint64_t snapshot = lifetime.Snapshot();
  EXPECT_TRUE(Lifetime::Alive(token, snapshot));
  lifetime.Invalidate();
  EXPECT_FALSE(Lifetime::Alive(token, snapshot));
  EXPECT_TRUE(lifetime.Alive(lifetime.Snapshot()));
}

TEST(LifetimeTest, InvalidateAndWaitWaitsForACallInProgressElsewhere) {
  Lifetime lifetime;
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  auto bound = lifetime.Bind([&entered, released]() {
    entered.set_value();
    released.wait();
  });
  std::thread caller([&bound]() { bound(); });
  entered.get_future().wait();

  std::atomic<bool> done{false};
  std::thread closer([&]() {
    lifetime.InvalidateAndWait();
    done = true;
  });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(done.load()) << "the call in progress holds it";
  release.set_value();
  caller.join();
  closer.join();
  EXPECT_TRUE(done.load());
}

TEST(LifetimeTest, InvalidateAndWaitFromInsideABoundCallDoesNotWaitOnItself) {
  Lifetime lifetime;
  bool returned = false;
  auto bound = lifetime.Bind([&]() {
    lifetime.InvalidateAndWait();
    returned = true;
  });
  auto ran = std::async(std::launch::async, bound);
  ASSERT_EQ(ran.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(returned);
}

TEST(LifetimeTest, AThrowingCallIsNoLongerInProgress) {
  Lifetime lifetime;
  auto bound = lifetime.Bind([]() { throw std::runtime_error("boom"); });
  EXPECT_THROW(bound(), std::runtime_error);
  auto closed = std::async(std::launch::async, [&lifetime]() { lifetime.InvalidateAndWait(); });
  EXPECT_EQ(closed.wait_for(5s), std::future_status::ready);
}

TEST(LifetimeTest, ConcurrentCallsAndAnInvalidationLeaveNoCallRunningAfterIt) {
  Lifetime lifetime;
  std::atomic<bool> closed{false};
  std::atomic<int> after_close{0};
  auto bound = lifetime.Bind([&]() {
    if (closed.load()) {
      ++after_close;
    }
  });
  std::atomic<bool> stop{false};
  std::vector<std::thread> callers;
  for (int i = 0; i < 4; ++i) {
    callers.emplace_back([&]() {
      while (!stop.load()) {
        bound();
      }
    });
  }
  std::this_thread::sleep_for(20ms);
  lifetime.InvalidateAndWait();
  closed = true;
  std::this_thread::sleep_for(20ms);
  stop = true;
  for (auto& caller : callers) {
    caller.join();
  }
  EXPECT_EQ(after_close.load(), 0) << "once it returned, no bound call ran";
}
