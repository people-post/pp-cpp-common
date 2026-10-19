#pragma once

#include "common/Executor.h"

#include <chrono>
#include <cstdint>

namespace pp {

/**
 * An executor that runs its tasks one at a time, on one thread: an owner thread, a library's control
 * lane (pp-cpp-mesh's), a test's queue. Besides posting it schedules (PostAfter) and lets a caller on
 * another thread run a lifecycle edge on it and wait (RunAndWait).
 *
 * RunsTasksOnCurrentThread is true on its thread, so its owner can assert affinity.
 */
class SerialExecutor : public Executor {
public:
  using TimerId = uint64_t;

  /**
   * Run `task` here once `delay` has passed on this executor's clock; 0 when it was not scheduled
   * (stopped). Any thread. A timer is dropped like a post when the executor stops.
   */
  virtual TimerId PostAfter(std::chrono::milliseconds delay, Task task) = 0;
  /** `id` does not run, unless it already started. No-op for 0 or a timer gone. Any thread. */
  virtual void CancelTimer(TimerId id) = 0;

  /**
   * Run `task` here and wait for it: a lifecycle edge (build, rewire, teardown) driven from another
   * thread. Inline when already here. The executor's own tasks must never wait on the caller — waits
   * go one way — so this cannot deadlock.
   *
   * When the executor drops the task instead (stopped), it runs on the caller: nothing runs here any
   * more. A task that throws is the executor's to report; the caller still returns.
   */
  virtual void RunAndWait(const Task& task);
};

} // namespace pp
