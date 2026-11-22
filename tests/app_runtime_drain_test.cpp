#include "common/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>

// AppRuntime::DrainWorkersThenUI is a completion barrier: every Critical / Normal worker task posted before it
// began, and the work those tasks post while they run, has finished before it returns true. The lanes are priority
// lanes of one multi-threaded pool, so a marker task proves nothing: it can run on a free thread while an earlier
// task still runs on another (pp-browser DeliveryOutboxTest on the macOS runner).

namespace pp {

/** Names the lane in test names (gtest and ctest). */
void PrintTo(const WorkerLane lane, std::ostream* os) {
  *os << (lane == WorkerLane::Critical ? "Critical" : lane == WorkerLane::Normal ? "Normal" : "Background");
}

} // namespace pp

namespace {

using namespace std::chrono_literals;
using pp::AppRuntime;
using pp::OwnerThreadId;
using pp::WorkerLane;

/** One-shot gate a task blocks on until the test opens it. */
class Gate {
public:
  void Open() {
    {
      std::lock_guard lock(mu_);
      open_ = true;
    }
    cv_.notify_all();
  }
  void Wait() {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [this] { return open_; });
  }

private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool open_ = false;
};

/** A worker task that blocks on its gate. Shared with the task, so a failing test cannot leave it dangling. */
struct BlockedTask {
  Gate gate;
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
};

std::shared_ptr<BlockedTask> PostBlocked(WorkerLane lane) {
  auto task = std::make_shared<BlockedTask>();
  AppRuntime::PostWorker(lane, [task] {
    task->started = true;
    task->gate.Wait();
    task->finished = true;
  });
  return task;
}

/** Until `done`, or false after `budget` (for the test's own bookkeeping, never for the property under test). */
template <typename Pred>
bool WaitFor(Pred&& done, std::chrono::milliseconds budget = 5s) {
  const auto until = std::chrono::steady_clock::now() + budget;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= until) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

class AppRuntimeDrainTest : public ::testing::TestWithParam<WorkerLane> {
protected:
  void SetUp() override {
    AppRuntime::InitializeUI();
    AppRuntime::Initialize(); // the default pool: several threads
  }
  void TearDown() override {
    AppRuntime::ReopenAfterTeardown();
    AppRuntime::Shutdown();
  }
};

// The bug: the marker ran on a free pool thread and the drain returned true with the earlier task still running.
TEST_P(AppRuntimeDrainTest, DoesNotReturnTrueWhileAnEarlierTaskStillRuns) {
  auto earlier = PostBlocked(GetParam());
  ASSERT_TRUE(WaitFor([&] { return earlier->started.load(); }));

  EXPECT_FALSE(AppRuntime::DrainWorkersThenUI(200ms)) << "the earlier task is still running: the budget passes first";
  EXPECT_FALSE(earlier->finished.load());

  earlier->gate.Open();
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(10s));
  EXPECT_TRUE(earlier->finished.load()) << "true means the earlier task has finished";
}

// The drain pumps the UI mailbox while it waits: a UI task (posted before it) releases the earlier task, and the
// drain returns true only once that task has finished.
TEST_P(AppRuntimeDrainTest, ReturnsTrueOnceTheEarlierTaskFinishes) {
  auto earlier = PostBlocked(GetParam());
  ASSERT_TRUE(WaitFor([&] { return earlier->started.load(); }));
  AppRuntime::PostUI([earlier] { earlier->gate.Open(); });

  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(10s));
  EXPECT_TRUE(earlier->finished.load());
}

// Work an earlier task posts while it runs (onto either lane, and what that posts in turn) is covered too, and so is
// a reply it posts to the UI mailbox.
TEST_P(AppRuntimeDrainTest, CoversWorkTheEarlierTasksPostWhileTheyRun) {
  auto gate = std::make_shared<Gate>();
  auto child = std::make_shared<std::atomic<bool>>(false);
  auto grandchild = std::make_shared<std::atomic<bool>>(false);
  auto reply = std::make_shared<std::atomic<bool>>(false);
  AppRuntime::PostWorker(GetParam(), [gate, child, grandchild] {
    gate->Wait();
    AppRuntime::PostWorkerCritical([child, grandchild] {
      AppRuntime::PostWorkerNormal([grandchild] {
        std::this_thread::sleep_for(20ms); // still running when every other pool thread is idle
        *grandchild = true;
      });
      *child = true;
    });
  });
  AppRuntime::PostWorkerAndReplyOnUI<int>(
      GetParam(),
      [gate] {
        gate->Wait();
        return 7;
      },
      [reply](int value) { *reply = value == 7; });
  AppRuntime::PostUI([gate] { gate->Open(); });

  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(10s));
  EXPECT_TRUE(child->load());
  EXPECT_TRUE(grandchild->load()) << "posted by a task that was itself posted by an earlier one";
  EXPECT_TRUE(reply->load()) << "the UI mailbox runs the earlier task's reply before the barrier";
}

// A busy app must not starve the drain: work posted after it began — a task that blocks, and a chain that keeps
// re-posting itself — is not waited for, from outside the pool or from a pool task posted after it began.
TEST_P(AppRuntimeDrainTest, DoesNotWaitForWorkPostedAfterItBegan) {
  auto earlier = PostBlocked(GetParam());
  ASSERT_TRUE(WaitFor([&] { return earlier->started.load(); }));
  std::shared_ptr<BlockedTask> later;
  auto stop_chain = std::make_shared<std::atomic<bool>>(false);
  auto chain_runs = std::make_shared<std::atomic<int>>(0);
  auto chain = std::make_shared<std::function<void()>>();
  *chain = [lane = GetParam(), stop_chain, chain_runs, weak = std::weak_ptr<std::function<void()>>(chain)] {
    ++*chain_runs;
    if (stop_chain->load()) {
      return;
    }
    if (auto next = weak.lock()) {
      AppRuntime::PostWorker(lane, [next] { (*next)(); });
    }
  };
  // Runs on this thread once the drain has begun (it pumps the UI mailbox): releases the earlier task and posts the
  // later work, both from outside the pool and from a pool task.
  AppRuntime::PostUI([&later, earlier, chain, lane = GetParam()] {
    later = PostBlocked(lane);
    AppRuntime::PostWorker(lane, [next = chain] { (*next)(); });
    earlier->gate.Open();
  });

  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(10s)) << "later work is not waited for";
  EXPECT_TRUE(earlier->finished.load());
  ASSERT_TRUE(later);
  EXPECT_FALSE(later->finished.load());

  later->gate.Open();
  *stop_chain = true;
  EXPECT_GT(chain_runs->load(), 0);
  ASSERT_TRUE(WaitFor([&] { return later->finished.load(); }));
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(10s)) << "the stopped chain's last links finish";
}

INSTANTIATE_TEST_SUITE_P(Lanes, AppRuntimeDrainTest, ::testing::Values(WorkerLane::Critical, WorkerLane::Normal),
                         [](const ::testing::TestParamInfo<WorkerLane>& info) {
                           return ::testing::PrintToString(info.param);
                         });

class AppRuntimeDrainOnceTest : public ::testing::Test {
protected:
  void TearDown() override {
    AppRuntime::ReopenAfterTeardown();
    AppRuntime::Shutdown();
  }
};

// Called from a pool task, the drain does not wait for the task that called it (it cannot finish first), but it
// does wait for the work that task posted before calling it.
TEST_F(AppRuntimeDrainOnceTest, FromAPoolTaskDoesNotWaitOnItsCaller) {
  AppRuntime::InitializeUI();
  AppRuntime::Initialize();
  struct Result {
    std::atomic<bool> done{false};
    bool drained = false;
    bool child_finished = false;
  };
  auto result = std::make_shared<Result>();
  AppRuntime::PostWorkerNormal([result] {
    auto child = std::make_shared<std::atomic<bool>>(false);
    AppRuntime::PostWorkerNormal([child] {
      std::this_thread::sleep_for(20ms);
      *child = true;
    });
    result->drained = AppRuntime::DrainWorkersThenUI(10s);
    result->child_finished = child->load();
    result->done = true;
  });
  ASSERT_TRUE(WaitFor([&] { return result->done.load(); }, 15s));
  EXPECT_TRUE(result->drained);
  EXPECT_TRUE(result->child_finished) << "posted by the caller before it drained";
}

// The Background lane is outside the barrier (long blocking work: a mesh's `blocking` executor).
TEST_F(AppRuntimeDrainOnceTest, BackgroundWorkIsNotWaitedFor) {
  AppRuntime::InitializeUI();
  AppRuntime::Initialize();
  auto background = PostBlocked(WorkerLane::Background);
  ASSERT_TRUE(WaitFor([&] { return background->started.load(); }));
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(10s));
  background->gate.Open();
  ASSERT_TRUE(WaitFor([&] { return background->finished.load(); }));
}

// A drain that runs out of budget behind an owner leaves no barrier task pointing into its returned frame: the
// next drain completes once the owner is free.
TEST_F(AppRuntimeDrainOnceTest, ABudgetPassedBehindAnOwnerLeavesTheNextDrainWorking) {
  AppRuntime::InitializeUI();
  pp::AppRuntimeConfig config;
  config.owners = {{OwnerThreadId{0}, "pp-test-owner"}};
  AppRuntime::Initialize(config);
  auto gate = std::make_shared<Gate>();
  AppRuntime::PostTo(OwnerThreadId{0}, [gate] { gate->Wait(); });

  EXPECT_FALSE(AppRuntime::DrainWorkersThenUI(50ms));
  gate->Open();
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(10s));
}

} // namespace
