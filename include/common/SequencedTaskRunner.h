#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "common/Executor.h"
#include "common/Module.h"

namespace pp {

/**
 * FIFO task queue drained inline on the UI thread via RunPendingTasks(). As an Executor its tasks run
 * on whichever thread drains it (an app's main loop; a test, by hand).
 */
class SequencedTaskRunner : public Executor, public Module {
public:
  SequencedTaskRunner();
  ~SequencedTaskRunner() override;

  SequencedTaskRunner(const SequencedTaskRunner&) = delete;
  SequencedTaskRunner& operator=(const SequencedTaskRunner&) = delete;

  void PostTask(std::function<void()> task);
  void Post(Task task) override { PostTask(std::move(task)); }
  bool RunsTasksOnCurrentThread() const override { return IsRunningOnThisThread(); }
  /** Run ahead of FIFO backlog (AcceptInvite must not wait behind Prefetch/circuit). */
  void PostTaskFront(std::function<void()> task);
  void RunPendingTasks();
  void Stop();
  /** Defer execution of queued/new tasks until Resume. Does not drop posted work. */
  void Pause();
  void Resume();

  bool IsRunningOnThisThread() const;
  bool HasPendingTasks() const;

private:
  void EnqueueLocked(std::function<void()> task);
  void EnqueueFrontLocked(std::function<void()> task);
  bool DequeueOne(std::function<void()>* out);
  void RunTaskSafely(std::function<void()>& task);

  mutable std::mutex mutex_;
  std::deque<std::function<void()>> tasks_;
  bool stopped_ = false;
  bool paused_ = false;
  std::thread::id thread_id_;
};

} // namespace pp

