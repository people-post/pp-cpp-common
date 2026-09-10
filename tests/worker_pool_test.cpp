#include "common/WorkerPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using pp::WorkerLane;
using pp::WorkerPool;

void WaitUntil(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL() << "Timed out waiting for condition";
}

} // namespace

TEST(WorkerPoolTest, CriticalRunsBeforeNormal) {
  WorkerPool pool(1);

  std::mutex mu;
  std::condition_variable cv;
  bool gate_open = false;
  std::vector<std::string> order;

  pool.Post(WorkerLane::Normal, [&]() {
    std::unique_lock lock(mu);
    cv.wait(lock, [&]() { return gate_open; });
    order.push_back("normal");
  });

  pool.Post(WorkerLane::Critical, [&]() { order.push_back("critical"); });

  WaitUntil([&]() { return order.size() == 1; }, std::chrono::milliseconds(2000));
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(order.front(), "critical");

  {
    std::lock_guard lock(mu);
    gate_open = true;
  }
  cv.notify_all();

  WaitUntil([&]() { return order.size() == 2; }, std::chrono::milliseconds(2000));
  EXPECT_EQ(order[0], "critical");
  EXPECT_EQ(order[1], "normal");

  pool.Shutdown();
}

TEST(WorkerPoolTest, NormalRunsBeforeBackground) {
  WorkerPool pool(1);

  std::mutex mu;
  std::condition_variable cv;
  bool gate_open = false;
  std::vector<std::string> order;

  pool.Post(WorkerLane::Background, [&]() {
    std::unique_lock lock(mu);
    cv.wait(lock, [&]() { return gate_open; });
    order.push_back("background");
  });

  pool.Post(WorkerLane::Normal, [&]() { order.push_back("normal"); });

  WaitUntil([&]() { return order.size() == 1; }, std::chrono::milliseconds(2000));
  EXPECT_EQ(order.front(), "normal");

  {
    std::lock_guard lock(mu);
    gate_open = true;
  }
  cv.notify_all();

  WaitUntil([&]() { return order.size() == 2; }, std::chrono::milliseconds(2000));
  EXPECT_EQ(order[1], "background");

  pool.Shutdown();
}

TEST(WorkerPoolTest, PostAndReplyReturnsResult) {
  WorkerPool pool(2);

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  int value = 0;

  pool.PostAndReply<int>(WorkerLane::Normal, []() { return 42; }, [&](int result) {
    value = result;
    {
      std::lock_guard lock(mu);
      done = true;
    }
    cv.notify_one();
  });

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2), [&]() { return done; }));
  }
  EXPECT_EQ(value, 42);

  pool.Shutdown();
}

TEST(WorkerPoolTest, PauseDefersQueuedWork) {
  WorkerPool pool(1);

  pool.Pause();
  std::atomic<int> ran{0};
  pool.Post(WorkerLane::Normal, [&]() { ran.fetch_add(1); });

  EXPECT_GE(pool.QueuedCount(WorkerLane::Normal), 1u);
  EXPECT_EQ(ran.load(), 0);

  pool.Resume();
  WaitUntil([&]() { return ran.load() == 1; }, std::chrono::milliseconds(2000));

  pool.Shutdown();
}

TEST(WorkerPoolTest, ShutdownDropsQueuedWorkAndJoins) {
  WorkerPool pool(1);

  std::mutex mu;
  std::condition_variable cv;
  bool started = false;
  bool allow_finish = false;
  std::atomic<int> background_ran{0};

  pool.Post(WorkerLane::Normal, [&]() {
    {
      std::lock_guard lock(mu);
      started = true;
    }
    cv.notify_all();
    std::unique_lock lock(mu);
    cv.wait(lock, [&]() { return allow_finish; });
  });

  WaitUntil([&]() {
    std::lock_guard lock(mu);
    return started;
  }, std::chrono::milliseconds(2000));

  // pool(1) clamps to kMinThreadCount (2). Pause *before* posting Background so the idle
  // second worker cannot dequeue it while Normal still holds the first.
  pool.Pause();
  pool.Post(WorkerLane::Background, [&]() { background_ran.fetch_add(1); });
  EXPECT_GE(pool.QueuedCount(WorkerLane::Background), 1u);

  {
    std::lock_guard lock(mu);
    allow_finish = true;
  }
  cv.notify_all();

  pool.Shutdown();
  EXPECT_EQ(background_ran.load(), 0);
}

TEST(WorkerPoolTest, PostAfterShutdownIsNoOp) {
  WorkerPool pool(1);
  pool.Shutdown();

  std::atomic<int> ran{0};
  pool.Post(WorkerLane::Critical, [&]() { ran.fetch_add(1); });
  EXPECT_EQ(pool.TotalQueuedCount(), 0u);
  EXPECT_EQ(ran.load(), 0);
}

TEST(WorkerPoolTest, ThreadCountIsClamped) {
  WorkerPool small_pool(1);
  WorkerPool large_pool(99);

  std::atomic<int> small_ran{0};
  std::atomic<int> large_ran{0};

  for (int i = 0; i < 4; ++i) {
    small_pool.Post(WorkerLane::Normal, [&]() { small_ran.fetch_add(1); });
    large_pool.Post(WorkerLane::Normal, [&]() { large_ran.fetch_add(1); });
  }

  WaitUntil([&]() { return small_ran.load() == 4 && large_ran.load() == 4; },
            std::chrono::milliseconds(3000));

  small_pool.Shutdown();
  large_pool.Shutdown();
}

TEST(WorkerPoolTest, ShutdownAbandonsJoinAfterBudget) {
  WorkerPool pool(1);

  std::mutex mu;
  std::condition_variable cv;
  bool started = false;
  std::atomic<bool> release{false};

  pool.Post(WorkerLane::Normal, [&]() {
    {
      std::lock_guard lock(mu);
      started = true;
    }
    cv.notify_all();
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  WaitUntil([&]() {
    std::lock_guard lock(mu);
    return started;
  }, std::chrono::milliseconds(2000));

  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(pool.Shutdown(std::chrono::milliseconds(100)));
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  EXPECT_LT(elapsed, std::chrono::milliseconds(1000));

  // Unblock the detached worker before process teardown so it does not touch freed stack.
  release.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

