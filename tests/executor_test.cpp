#include "common/Executor.h"
#include "common/SequencedTaskRunner.h"
#include "common/ThreadExecutor.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

TEST(ThreadExecutorTest, RunsTasksInOrderOnItsOwnThread) {
  pp::ThreadExecutor executor;
  std::vector<int> order;
  std::promise<void> done;
  std::atomic<bool> onItsThread{false};
  for (int i = 0; i < 5; ++i) {
    executor.Post([&order, i]() { order.push_back(i); });
  }
  executor.Post([&]() {
    onItsThread = executor.RunsTasksOnCurrentThread();
    done.set_value();
  });
  ASSERT_EQ(done.get_future().wait_for(5s), std::future_status::ready);
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
  EXPECT_TRUE(onItsThread.load());
  EXPECT_FALSE(executor.RunsTasksOnCurrentThread());
}

TEST(ThreadExecutorTest, AThrowingTaskIsLoggedAndTheThreadRunsOn) {
  pp::ThreadExecutor executor;
  std::promise<void> after;
  executor.Post([]() { throw std::runtime_error("boom"); });
  executor.Post([&after]() { after.set_value(); });
  EXPECT_EQ(after.get_future().wait_for(5s), std::future_status::ready);
}

TEST(ThreadExecutorTest, StopDropsWhatIsQueuedAndLaterPosts) {
  pp::ThreadExecutor executor;
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<int> ran{0};
  executor.Post([&entered, released]() {
    entered.set_value();
    released.wait();
  });
  entered.get_future().wait();
  executor.Post([&ran]() { ++ran; }); // queued behind the running task
  std::thread stopper([&executor]() { executor.Stop(); });
  std::this_thread::sleep_for(20ms);
  release.set_value(); // the running task finishes; Stop joins
  stopper.join();
  executor.Post([&ran]() { ++ran; });
  EXPECT_EQ(ran.load(), 0);
  executor.Stop(); // idempotent
}

TEST(ExecutorTest, AWorkerPoolLaneIsAnExecutor) {
  pp::WorkerPool pool(2);
  pp::WorkerLaneExecutor background(pool, pp::WorkerLane::Background);
  pp::Executor& executor = background;
  std::promise<void> ran;
  executor.Post([&ran]() { ran.set_value(); });
  EXPECT_EQ(ran.get_future().wait_for(5s), std::future_status::ready);
  EXPECT_FALSE(executor.RunsTasksOnCurrentThread()) << "a pool lane is not one thread";
  pool.Shutdown();
}

TEST(ExecutorTest, ASequencedTaskRunnerIsAnExecutorDrainedByItsThread) {
  pp::SequencedTaskRunner runner;
  pp::Executor& executor = runner;
  int ran = 0;
  bool onIt = false;
  executor.Post([&]() {
    ++ran;
    onIt = executor.RunsTasksOnCurrentThread();
  });
  EXPECT_EQ(ran, 0) << "Post never runs inline";
  runner.RunPendingTasks();
  EXPECT_EQ(ran, 1);
  EXPECT_TRUE(onIt);
}
