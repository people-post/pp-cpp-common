#include "common/ThreadExecutor.h"

#include <algorithm>

namespace pp {

ThreadExecutor::ThreadExecutor() : thread_([this]() { Run(); }) { redirectLogger("ThreadExecutor"); }

ThreadExecutor::~ThreadExecutor() { Stop(); }

void ThreadExecutor::Post(Task task) {
  if (!task) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return;
    }
    tasks_.push_back(std::move(task));
  }
  wake_.notify_one();
}

bool ThreadExecutor::RunsTasksOnCurrentThread() const { return std::this_thread::get_id() == thread_.get_id(); }

SerialExecutor::TimerId ThreadExecutor::PostAfter(const std::chrono::milliseconds delay, Task task) {
  if (!task) {
    return 0;
  }
  TimerId id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return 0;
    }
    id = next_timer_++;
    deadlines_.emplace(Clock::now() + std::max(delay, std::chrono::milliseconds(0)), id);
    timers_.emplace(id, std::move(task));
  }
  wake_.notify_one();
  return id;
}

void ThreadExecutor::CancelTimer(const TimerId id) {
  Task cancelled;  // destroyed outside the lock: it may own anything
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto it = timers_.find(id); it != timers_.end()) {
    cancelled = std::move(it->second);
    timers_.erase(it);
  }
}

void ThreadExecutor::QueueDueTimersLocked(const Clock::time_point now) {
  while (!deadlines_.empty() && deadlines_.begin()->first <= now) {
    const TimerId id = deadlines_.begin()->second;
    deadlines_.erase(deadlines_.begin());
    if (auto it = timers_.find(id); it != timers_.end()) {
      tasks_.push_back(std::move(it->second));
      timers_.erase(it);
    }
  }
}

void ThreadExecutor::Stop() {
  // Dropped outside the lock: a task may own anything, even something that posts here as it goes.
  std::deque<Task> dropped;
  std::unordered_map<TimerId, Task> dropped_timers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    dropped.swap(tasks_);
    dropped_timers.swap(timers_);
    deadlines_.clear();
  }
  wake_.notify_one();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void ThreadExecutor::Run() {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      for (;;) {
        if (stopped_) {
          return;
        }
        QueueDueTimersLocked(Clock::now());
        if (!tasks_.empty()) {
          break;
        }
        if (deadlines_.empty()) {
          wake_.wait(lock);
        } else {
          // A copy: wait_until reads its deadline again after waking, when Stop may have cleared the set.
          const Clock::time_point next = deadlines_.begin()->first;
          wake_.wait_until(lock, next);
        }
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    try {
      task();
    } catch (const std::exception& e) {
      log().error << "Uncaught exception in task: " << e.what();
    } catch (...) {
      log().error << "Uncaught unknown exception in task";
    }
  }
}

} // namespace pp
