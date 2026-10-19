#pragma once

#include "common/SerialExecutor.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

namespace pp {

/**
 * A serial executor a test drives: nothing runs until the test calls RunPending or AdvanceBy, on the
 * test's thread, and time is the executor's own — it moves only by AdvanceBy. For deterministic
 * tests of code that takes its executors (several nodes in one process, each on its own).
 *
 * Any thread may post or cancel; one thread drives at a time.
 */
class ManualExecutor final : public SerialExecutor {
public:
  ManualExecutor() = default;
  ManualExecutor(const ManualExecutor&) = delete;
  ManualExecutor& operator=(const ManualExecutor&) = delete;

  void Post(Task task) override;
  /** True while a task runs (on the driving thread). */
  bool RunsTasksOnCurrentThread() const override;
  /** Due at Now() + delay. */
  TimerId PostAfter(std::chrono::milliseconds delay, Task task) override;
  void CancelTimer(TimerId id) override;
  /** Inline when called from a task; else posts it and drains the queue on the caller. */
  void RunAndWait(const Task& task) override;

  /** Run queued tasks, and what they post, until none is left. Returns how many ran. */
  size_t RunPending();
  /**
   * Move time forward by `by`: timers run in deadline order, each at its deadline (Now() reads it),
   * with the queue drained after each; then the queue is drained at the end time.
   */
  size_t AdvanceBy(std::chrono::milliseconds by);
  /** Time since construction, on this executor's clock. */
  std::chrono::milliseconds Now() const;
  /** Queued tasks and timers not yet run. */
  bool Idle() const;

private:
  /** Pops the next queued task (false when none). */
  bool PopTask(Task& task);
  void RunOne(Task& task);

  mutable std::mutex mutex_;
  std::deque<Task> tasks_;
  /** Deadline order (ms on this clock); an entry whose id left `timers_` was cancelled. */
  std::set<std::pair<int64_t, TimerId>> deadlines_;
  std::unordered_map<TimerId, Task> timers_;
  TimerId next_timer_{1};
  int64_t now_ms_{0};
  std::atomic<std::thread::id> running_{};
};

} // namespace pp
