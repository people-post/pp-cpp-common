#include "common/ManualExecutor.h"
#include "common/SerialExecutor.h"
#include "common/ThreadExecutor.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// --- ThreadExecutor as a SerialExecutor ------------------------------------------------------------

TEST(ThreadExecutorTimerTest, TimersRunInDeadlineOrderOnItsThread) {
  pp::ThreadExecutor executor;
  std::vector<int> order;
  std::promise<void> done;
  std::atomic<bool> onItsThread{false};
  executor.PostAfter(60ms, [&]() {
    order.push_back(3);
    onItsThread = executor.RunsTasksOnCurrentThread();
    done.set_value();
  });
  executor.PostAfter(20ms, [&order]() { order.push_back(2); });
  executor.Post([&order]() { order.push_back(1); });
  ASSERT_EQ(done.get_future().wait_for(5s), std::future_status::ready);
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_TRUE(onItsThread.load());
}

TEST(ThreadExecutorTimerTest, ATimerRunsNoEarlierThanItsDelay) {
  pp::ThreadExecutor executor;
  std::promise<std::chrono::steady_clock::time_point> fired;
  const auto posted = std::chrono::steady_clock::now();
  executor.PostAfter(50ms, [&fired]() { fired.set_value(std::chrono::steady_clock::now()); });
  auto when = fired.get_future();
  ASSERT_EQ(when.wait_for(5s), std::future_status::ready);
  EXPECT_GE(when.get() - posted, 50ms);
}

TEST(ThreadExecutorTimerTest, ACancelledTimerDoesNotRun) {
  pp::ThreadExecutor executor;
  std::atomic<int> ran{0};
  const auto id = executor.PostAfter(30ms, [&ran]() { ++ran; });
  ASSERT_NE(id, 0u);
  executor.CancelTimer(id);
  executor.CancelTimer(id); // gone: no-op
  executor.CancelTimer(0);
  std::promise<void> later;
  executor.PostAfter(80ms, [&later]() { later.set_value(); });
  ASSERT_EQ(later.get_future().wait_for(5s), std::future_status::ready);
  EXPECT_EQ(ran.load(), 0);
}

TEST(ThreadExecutorTimerTest, StopDropsTimersAndRefusesNewOnes) {
  pp::ThreadExecutor executor;
  std::atomic<int> ran{0};
  executor.PostAfter(20ms, [&ran]() { ++ran; });
  executor.Stop();
  EXPECT_EQ(executor.PostAfter(1ms, [&ran]() { ++ran; }), 0u);
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(ran.load(), 0);
}

TEST(ThreadExecutorRunAndWaitTest, RunsOnItsThreadAndReturnsAfter) {
  pp::ThreadExecutor executor;
  bool ran = false;
  bool onItsThread = false;
  executor.RunAndWait([&]() {
    std::this_thread::sleep_for(20ms);
    onItsThread = executor.RunsTasksOnCurrentThread();
    ran = true;
  });
  EXPECT_TRUE(ran);
  EXPECT_TRUE(onItsThread);
}

TEST(ThreadExecutorRunAndWaitTest, RunsAfterWhatWasQueued) {
  pp::ThreadExecutor executor;
  std::vector<int> order;
  executor.Post([&order]() {
    std::this_thread::sleep_for(20ms);
    order.push_back(1);
  });
  executor.RunAndWait([&order]() { order.push_back(2); });
  EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(ThreadExecutorRunAndWaitTest, InlineFromItsOwnTask) {
  pp::ThreadExecutor executor;
  std::vector<int> order;
  executor.RunAndWait([&]() {
    executor.RunAndWait([&order]() { order.push_back(1); }); // would wait on itself if posted
    order.push_back(2);
  });
  EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(ThreadExecutorRunAndWaitTest, OnTheCallerOnceStopped) {
  pp::ThreadExecutor executor;
  executor.Stop();
  bool ran = false;
  std::thread::id ranOn;
  executor.RunAndWait([&]() {
    ran = true;
    ranOn = std::this_thread::get_id();
  });
  EXPECT_TRUE(ran);
  EXPECT_EQ(ranOn, std::this_thread::get_id());
}

TEST(ThreadExecutorRunAndWaitTest, OnTheCallerWhenStoppedWhileQueued) {
  pp::ThreadExecutor executor;
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  executor.Post([&entered, released]() {
    entered.set_value();
    released.wait();
  });
  entered.get_future().wait();
  std::atomic<bool> ran{false};
  std::thread caller([&executor, &ran]() { executor.RunAndWait([&ran]() { ran = true; }); });
  std::this_thread::sleep_for(20ms); // the caller's task is queued behind the running one
  std::thread stopper([&executor]() { executor.Stop(); });
  std::this_thread::sleep_for(20ms);
  release.set_value();
  stopper.join();
  caller.join(); // dropped by Stop: ran on the caller, which returned
  EXPECT_TRUE(ran.load());
}

TEST(ThreadExecutorRunAndWaitTest, AThrowingTaskStillReturnsToTheCaller) {
  pp::ThreadExecutor executor;
  executor.RunAndWait([]() { throw std::runtime_error("boom"); });
  bool after = false;
  executor.RunAndWait([&after]() { after = true; });
  EXPECT_TRUE(after);
}

// --- ManualExecutor --------------------------------------------------------------------------------

TEST(ManualExecutorTest, NothingRunsUntilTheTestDrives) {
  pp::ManualExecutor executor;
  std::vector<int> order;
  executor.Post([&]() {
    order.push_back(1);
    executor.Post([&order]() { order.push_back(3); }); // posted while draining: same drain
  });
  executor.Post([&order]() { order.push_back(2); });
  EXPECT_TRUE(order.empty());
  EXPECT_FALSE(executor.Idle());
  EXPECT_EQ(executor.RunPending(), 3u);
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_TRUE(executor.Idle());
}

TEST(ManualExecutorTest, OnItsThreadOnlyWhileATaskRuns) {
  pp::ManualExecutor executor;
  bool inside = false;
  executor.Post([&]() { inside = executor.RunsTasksOnCurrentThread(); });
  EXPECT_FALSE(executor.RunsTasksOnCurrentThread());
  executor.RunPending();
  EXPECT_TRUE(inside);
  EXPECT_FALSE(executor.RunsTasksOnCurrentThread());
}

TEST(ManualExecutorTest, TimersRunAtTheirDeadlineOnItsClock) {
  pp::ManualExecutor executor;
  std::vector<std::pair<int, std::chrono::milliseconds>> fired;
  executor.PostAfter(300ms, [&]() { fired.emplace_back(3, executor.Now()); });
  executor.PostAfter(100ms, [&]() {
    fired.emplace_back(1, executor.Now());
    executor.PostAfter(50ms, [&]() { fired.emplace_back(2, executor.Now()); }); // due at 150
  });
  executor.AdvanceBy(99ms);
  EXPECT_TRUE(fired.empty());
  executor.AdvanceBy(201ms);
  ASSERT_EQ(fired.size(), 3u);
  EXPECT_EQ(fired[0], std::make_pair(1, 100ms));
  EXPECT_EQ(fired[1], std::make_pair(2, 150ms));
  EXPECT_EQ(fired[2], std::make_pair(3, 300ms));
  EXPECT_EQ(executor.Now(), 300ms);
}

TEST(ManualExecutorTest, TasksATimerPostsRunBeforeTheNextTimer) {
  pp::ManualExecutor executor;
  std::vector<int> order;
  executor.PostAfter(10ms, [&]() {
    order.push_back(1);
    executor.Post([&order]() { order.push_back(2); });
  });
  executor.PostAfter(20ms, [&order]() { order.push_back(3); });
  executor.AdvanceBy(1s);
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(ManualExecutorTest, ACancelledTimerDoesNotRun) {
  pp::ManualExecutor executor;
  int ran = 0;
  const auto id = executor.PostAfter(10ms, [&ran]() { ++ran; });
  executor.CancelTimer(id);
  executor.AdvanceBy(1s);
  EXPECT_EQ(ran, 0);
  EXPECT_TRUE(executor.Idle());
}

TEST(ManualExecutorTest, RunAndWaitDrainsInOrderOnTheCaller) {
  pp::ManualExecutor executor;
  std::vector<int> order;
  executor.Post([&order]() { order.push_back(1); });
  executor.RunAndWait([&]() {
    order.push_back(2);
    executor.RunAndWait([&order]() { order.push_back(3); }); // from a task: inline
  });
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(SerialExecutorTest, BothAreSerialExecutors) {
  pp::ThreadExecutor thread;
  pp::ManualExecutor manual;
  for (pp::SerialExecutor* executor : {static_cast<pp::SerialExecutor*>(&thread), static_cast<pp::SerialExecutor*>(&manual)}) {
    int ran = 0;
    executor->RunAndWait([&ran]() { ++ran; });
    EXPECT_EQ(ran, 1);
  }
}
