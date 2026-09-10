#pragma once

#include "common/Module.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pp {

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
  static size_t ClampThreadCount(size_t thread_count);
  void WorkerMain(size_t worker_index);
  bool DequeueOneLocked(std::function<void()>* out);
  bool HasWorkLocked() const;
  void EnqueueLocked(WorkerLane lane, std::function<void()> task);
  void RunTaskSafely(std::function<void()>& task);

  const size_t thread_count_;
  std::vector<std::thread> threads_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> critical_queue_;
  std::deque<std::function<void()>> normal_queue_;
  std::deque<std::function<void()>> background_queue_;
  bool stopped_ = false;
  bool paused_ = false;
  std::atomic<size_t> live_workers_{0};
};

template <typename Result>
void WorkerPool::PostAndReply(WorkerLane lane, std::function<Result()> work,
                              std::function<void(Result)> on_done) {
  Post(lane, [work = std::move(work), on_done = std::move(on_done)]() mutable {
    Result result = work();
    if (on_done) {
      on_done(std::move(result));
    }
  });
}

} // namespace pp

