#pragma once

#include "common/Executor.h"
#include "common/Module.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace pp {

/**
 * A serial executor with a thread of its own: tasks run one at a time, in the order posted. For a
 * host with no runtime of its own to offer (tools, a library's tests); an app with one adapts its
 * own lanes instead. A task that throws is logged; the thread runs on.
 */
class ThreadExecutor final : public Executor, public Module {
public:
  ThreadExecutor();
  /** Stop(). */
  ~ThreadExecutor() override;

  ThreadExecutor(const ThreadExecutor&) = delete;
  ThreadExecutor& operator=(const ThreadExecutor&) = delete;

  void Post(Task task) override;
  bool RunsTasksOnCurrentThread() const override;

  /**
   * Runs no task posted from now on, lets the one running finish and joins the thread; queued tasks
   * are dropped. Not on the executor's own thread (it would wait on itself). Idempotent.
   */
  void Stop();

private:
  void Run();

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Task> tasks_;
  bool stopped_{false};
  std::thread thread_;
};

} // namespace pp
