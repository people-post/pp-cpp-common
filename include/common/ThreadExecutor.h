#pragma once

#include "common/Module.h"
#include "common/SerialExecutor.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

namespace pp {

/**
 * A serial executor with a thread of its own: tasks run one at a time, in the order posted; timers on
 * the steady clock run in deadline order once due. For a host with no runtime of its own to offer
 * (tools, a library's tests); an app with one adapts its own lanes instead. A task that throws is
 * logged; the thread runs on.
 */
class ThreadExecutor final : public SerialExecutor, public Module {
public:
  ThreadExecutor();
  /** Stop(). */
  ~ThreadExecutor() override;

  ThreadExecutor(const ThreadExecutor&) = delete;
  ThreadExecutor& operator=(const ThreadExecutor&) = delete;

  void Post(Task task) override;
  bool RunsTasksOnCurrentThread() const override;
  TimerId PostAfter(std::chrono::milliseconds delay, Task task) override;
  void CancelTimer(TimerId id) override;

  /**
   * Runs no task posted from now on, lets the one running finish and joins the thread; queued tasks
   * and timers are dropped. Not on the executor's own thread (it would wait on itself). Idempotent.
   */
  void Stop();

private:
  using Clock = std::chrono::steady_clock;

  void Run();
  /** Queue the timers due by `now`, in deadline order. */
  void QueueDueTimersLocked(Clock::time_point now);

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Task> tasks_;
  /** Deadline order; an entry whose id left `timers_` was cancelled. */
  std::set<std::pair<Clock::time_point, TimerId>> deadlines_;
  std::unordered_map<TimerId, Task> timers_;
  TimerId next_timer_{1};
  bool stopped_{false};
  std::thread thread_;
};

} // namespace pp
