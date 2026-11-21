#pragma once

#include "common/Module.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pp {

class AppRuntime;

enum class WorkerLane { Critical, Normal, Background };

/** Fixed-size pool with three priority lanes. Only pool threads may run long blocking work. */
class WorkerPool : public Module {
public:
  /** Floor for HTTP / LLM / Argon2 concurrency. Amp Connect waits use MeshControlPool (pp-browser). */
  static constexpr size_t kMinThreadCount = 2;
  static constexpr size_t kMaxThreadCount = 8;
  static constexpr size_t kDefaultThreadCount = 4;
  /** Soft join budget for product quit (stuck in-flight work is detached until process exit). */
  static constexpr std::chrono::milliseconds kDefaultShutdownJoinBudget{500};

  explicit WorkerPool(size_t thread_count = kDefaultThreadCount);
  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  void Post(WorkerLane lane, std::function<void()> task);
  /** Runs work on a pool thread; on_done is invoked on that same thread (post to UI/coordinator if needed). */
  template <typename Result>
  void PostAndReply(WorkerLane lane, std::function<Result()> work, std::function<void(Result)> on_done);

  void Pause();
  void Resume();
  /** Stop accepting work, drop queued tasks, join workers (in-flight tasks still finish). */
  void Shutdown() { (void)Shutdown(kDefaultShutdownJoinBudget); }
  /**
   * Like Shutdown(), but abandon join after `join_budget` if a worker is stuck mid-task.
   * Detached workers are leaked until process exit — only safe when the process is quitting.
   * Returns false if any worker was detached after the budget.
   */
  bool Shutdown(std::chrono::milliseconds join_budget);

  size_t QueuedCount(WorkerLane lane) const;
  size_t TotalQueuedCount() const;

private:
  // AppRuntime::DrainWorkersThenUI waits on a CompletionBarrier (below); it is not part of the pool's own API.
  friend class AppRuntime;

  /**
   * Tracking for CompletionBarrier. Every post takes the next ticket (one sequence across the lanes). A Critical /
   * Normal task is *counted*: from its post until it has finished (run, and its captures released) or Shutdown
   * dropped it, it is in SharedState::outstanding under its *origin*: its own ticket, or — when it was posted from a
   * counted task of this pool while that task ran — that task's origin. So origin <= ticket, and a task's origin is
   * older than any later post's ticket. Background tasks are not counted.
   */
  struct QueuedTask {
    std::function<void()> run;
    bool counted = false;
    uint64_t origin = 0;
  };
  /** The pool task this thread is running (none: `pool` is null). */
  struct RunningTask {
    const void* pool = nullptr;
    bool counted = false;
    uint64_t origin = 0;
  };
  /** Set while a pool thread runs a task; cleared while a CompletionBarrier lives on the thread. */
  static thread_local RunningTask running_on_this_thread_;

  // Everything a worker thread touches after Shutdown()'s join budget expires
  // lives here instead of on the WorkerPool, and each worker thread captures
  // a shared_ptr to it. If Shutdown() has to detach a stuck worker (see
  // Shutdown()'s doc comment), the WorkerPool — and thus its mutex_/cv_/queues
  // — may be destroyed while that thread is still running; keeping this state
  // alive via shared_ptr instead means the detached thread never touches
  // freed memory, at the cost of leaking this block until the thread exits.
  struct SharedState {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<QueuedTask> critical_queue;
    std::deque<QueuedTask> normal_queue;
    std::deque<QueuedTask> background_queue;
    /** Ticket of the next post (QueuedTask). */
    uint64_t next_ticket = 0;
    /** Counted tasks queued or running, by origin (QueuedTask). */
    std::map<uint64_t, size_t> outstanding;
    /** Notified when a counted task settles while a CompletionBarrier waits. */
    std::condition_variable settled_cv;
    size_t barrier_waiters = 0;
    bool stopped = false;
    bool paused = false;
    std::atomic<size_t> live_workers{0};
    // Independent copy (not a reference to the WorkerPool's Module::log()) so
    // a detached worker can still log after the WorkerPool is gone.
    logging::Logger logger;

    explicit SharedState(logging::Logger log) : logger(std::move(log)) {}
  };

  /**
   * Completion barrier over Critical / Normal work: Wait() is true once every counted task posted before the barrier
   * was made has finished, and so has every task those posted while they ran (transitively) — the counted tasks
   * whose origin is below the barrier's ticket. Work posted after the barrier was made by anything else (another
   * thread, or a pool task that is not one of those) is not waited for, so a busy pool cannot starve it.
   *
   * Why that set is exactly "the earlier work and what it posted", and why it drains: a task with an origin below the
   * ticket is either posted before the barrier (origin <= ticket) or posted, after it, from a running task that is
   * itself in the set (a post inherits the origin of the counted task running on its thread). Nothing else can add to
   * the set, so once it is empty it stays empty; while it is not, Wait() keeps waiting (up to its argument). A chain
   * of earlier work that re-posts itself forever keeps it non-empty: the caller's budget then passes.
   *
   * Made on a thread that is running one of this pool's counted tasks, the barrier leaves that task out (it cannot
   * finish while its thread waits) but waits for what it posted before; while the barrier lives, posts from this
   * thread are new work (not the calling task's), so tasks the waiting thread runs meanwhile (UI, Manual owners) do
   * not extend it. Two pool tasks that wait on barriers at once each wait for the other, until their budgets pass.
   * The pool must keep a free thread for the earlier work to run on (it has two at least; the caller holds one).
   */
  class CompletionBarrier {
  public:
    explicit CompletionBarrier(WorkerPool& pool);
    ~CompletionBarrier();
    CompletionBarrier(const CompletionBarrier&) = delete;
    CompletionBarrier& operator=(const CompletionBarrier&) = delete;

    /** Up to `wait` for the earlier work to finish; true once it has. */
    bool Wait(std::chrono::milliseconds wait);

  private:
    bool SettledLocked() const;

    const std::shared_ptr<SharedState> state_;
    const RunningTask caller_;
    uint64_t ticket_ = 0;
  };

  static size_t ClampThreadCount(size_t thread_count);
  static void WorkerMain(std::shared_ptr<SharedState> state, size_t worker_index);
  static bool DequeueOneLocked(SharedState& state, QueuedTask* out);
  static bool HasWorkLocked(const SharedState& state);
  static void SettleLocked(SharedState& state, uint64_t origin);
  static void RunTaskSafely(logging::Logger& logger, std::function<void()>& task);

  const size_t thread_count_;
  const std::shared_ptr<SharedState> state_;
  std::vector<std::thread> threads_;
};

template <typename Result>
void WorkerPool::PostAndReply(WorkerLane lane, std::function<Result()> work, std::function<void(Result)> on_done) {
  Post(lane, [work = std::move(work), on_done = std::move(on_done)]() mutable {
    Result result = work();
    if (on_done) {
      on_done(std::move(result));
    }
  });
}

} // namespace pp
