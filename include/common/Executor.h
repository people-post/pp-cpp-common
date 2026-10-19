#pragma once

#include "common/WorkerPool.h"

#include <functional>

namespace pp {

/**
 * Somewhere tasks run — a thread, a lane of a pool, an app's main loop. Libraries take the executors
 * they run on from the app (pp-cpp-mesh's control lane and workers) instead of reaching for its
 * threads.
 *
 * Post never blocks and never runs the task inline; a task posted after the executor stopped may be
 * dropped. Any thread.
 */
class Executor {
public:
  using Task = std::function<void()>;

  virtual ~Executor() = default;

  virtual void Post(Task task) = 0;
  /**
   * True on the thread this executor runs its tasks on, so a serial executor's owner can assert
   * affinity. Executors spread over several threads (a pool lane) answer false.
   */
  virtual bool RunsTasksOnCurrentThread() const { return false; }
};

/** One lane of a WorkerPool as an Executor. The pool must outlive it. */
class WorkerLaneExecutor final : public Executor {
public:
  WorkerLaneExecutor(WorkerPool& pool, WorkerLane lane) : pool_(pool), lane_(lane) {}

  void Post(Task task) override { pool_.Post(lane_, std::move(task)); }

private:
  WorkerPool& pool_;
  WorkerLane lane_;
};

} // namespace pp
