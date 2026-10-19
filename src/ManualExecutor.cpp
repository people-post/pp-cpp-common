#include "common/ManualExecutor.h"

#include <algorithm>

namespace pp {

void ManualExecutor::Post(Task task) {
  if (!task) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  tasks_.push_back(std::move(task));
}

bool ManualExecutor::RunsTasksOnCurrentThread() const {
  return running_.load(std::memory_order_acquire) == std::this_thread::get_id();
}

SerialExecutor::TimerId ManualExecutor::PostAfter(const std::chrono::milliseconds delay, Task task) {
  if (!task) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const TimerId id = next_timer_++;
  deadlines_.emplace(now_ms_ + std::max<int64_t>(delay.count(), 0), id);
  timers_.emplace(id, std::move(task));
  return id;
}

void ManualExecutor::CancelTimer(const TimerId id) {
  Task cancelled; // destroyed outside the lock: it may own anything
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto it = timers_.find(id); it != timers_.end()) {
    cancelled = std::move(it->second);
    timers_.erase(it);
  }
}

void ManualExecutor::RunAndWait(const Task& task) {
  if (!task) {
    return;
  }
  if (RunsTasksOnCurrentThread()) {
    task();
    return;
  }
  // In order: what was queued before runs first, as on a thread of its own.
  Post([&task]() { task(); });
  RunPending();
}

size_t ManualExecutor::RunPending() {
  size_t ran = 0;
  Task task;
  while (PopTask(task)) {
    RunOne(task);
    ++ran;
  }
  return ran;
}

size_t ManualExecutor::AdvanceBy(const std::chrono::milliseconds by) {
  size_t ran = RunPending();
  int64_t end = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    end = now_ms_ + std::max<int64_t>(by.count(), 0);
  }
  for (;;) {
    Task timer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Cancelled entries first leave the order; then the earliest live deadline, if due.
      while (!deadlines_.empty() && timers_.find(deadlines_.begin()->second) == timers_.end()) {
        deadlines_.erase(deadlines_.begin());
      }
      if (deadlines_.empty() || deadlines_.begin()->first > end) {
        now_ms_ = end;
        break;
      }
      const auto [deadline, id] = *deadlines_.begin();
      deadlines_.erase(deadlines_.begin());
      now_ms_ = std::max(now_ms_, deadline);
      auto it = timers_.find(id);
      timer = std::move(it->second);
      timers_.erase(it);
    }
    RunOne(timer);
    ran += 1 + RunPending();
  }
  return ran + RunPending();
}

std::chrono::milliseconds ManualExecutor::Now() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::chrono::milliseconds(now_ms_);
}

bool ManualExecutor::Idle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.empty() && timers_.empty();
}

bool ManualExecutor::PopTask(Task& task) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (tasks_.empty()) {
    return false;
  }
  task = std::move(tasks_.front());
  tasks_.pop_front();
  return true;
}

void ManualExecutor::RunOne(Task& task) {
  // Nested drives (RunAndWait from a task runs inline instead) keep the outer driver's identity.
  const std::thread::id outer = running_.exchange(std::this_thread::get_id(), std::memory_order_acq_rel);
  struct Restore {
    std::atomic<std::thread::id>& running;
    std::thread::id outer;
    ~Restore() { running.store(outer, std::memory_order_release); }
  } restore{running_, outer};
  Task run = std::move(task);
  task = nullptr;
  run();
}

} // namespace pp
