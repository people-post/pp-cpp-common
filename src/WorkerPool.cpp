#include "common/WorkerPool.h"

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <string>

#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#endif

namespace pp {

thread_local WorkerPool::RunningTask WorkerPool::running_on_this_thread_{};

size_t WorkerPool::ClampThreadCount(const size_t thread_count) {
  if (thread_count < kMinThreadCount) {
    return kMinThreadCount;
  }
  return std::min(thread_count, kMaxThreadCount);
}

WorkerPool::WorkerPool(const size_t thread_count)
    : thread_count_(ClampThreadCount(thread_count)), state_(std::make_shared<SharedState>(log())) {
  redirectLogger("WorkerPool");
  state_->logger = log(); // refresh the copy now that redirectLogger has run
  state_->live_workers.store(thread_count_, std::memory_order_relaxed);
  threads_.reserve(thread_count_);
  for (size_t i = 0; i < thread_count_; ++i) {
    threads_.emplace_back([state = state_, i]() { WorkerMain(state, i); });
  }
}

WorkerPool::~WorkerPool() {
  (void)Shutdown(kDefaultShutdownJoinBudget);
}

void WorkerPool::Post(WorkerLane lane, std::function<void()> task) {
  if (!task) {
    return;
  }

  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopped) {
      return;
    }
    QueuedTask queued{std::move(task), lane != WorkerLane::Background, 0};
    const uint64_t ticket = state_->next_ticket++;
    if (queued.counted) {
      const RunningTask& poster = running_on_this_thread_;
      queued.origin = poster.pool == state_.get() && poster.counted ? poster.origin : ticket;
      ++state_->outstanding[queued.origin];
    }
    switch (lane) {
      case WorkerLane::Critical:
        state_->critical_queue.push_back(std::move(queued));
        break;
      case WorkerLane::Normal:
        state_->normal_queue.push_back(std::move(queued));
        break;
      case WorkerLane::Background:
        state_->background_queue.push_back(std::move(queued));
        break;
    }
  }
  state_->cv.notify_one();
}

void WorkerPool::Pause() {
  std::lock_guard lock(state_->mutex);
  state_->paused = true;
}

void WorkerPool::Resume() {
  {
    std::lock_guard lock(state_->mutex);
    state_->paused = false;
  }
  state_->cv.notify_all();
}

bool WorkerPool::Shutdown(std::chrono::milliseconds join_budget) {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopped) {
      return state_->live_workers.load(std::memory_order_acquire) == 0 && threads_.empty();
    }
    state_->stopped = true;
    for (auto* queue : {&state_->critical_queue, &state_->normal_queue, &state_->background_queue}) {
      for (const QueuedTask& dropped : *queue) {
        if (dropped.counted) {
          SettleLocked(*state_, dropped.origin);
        }
      }
      queue->clear();
    }
  }
  state_->cv.notify_all();

  const auto deadline = std::chrono::steady_clock::now() + join_budget;
  {
    std::unique_lock lock(state_->mutex);
    state_->cv.wait_until(lock, deadline,
                          [this]() { return state_->live_workers.load(std::memory_order_acquire) == 0; });
  }

  const bool all_exited = state_->live_workers.load(std::memory_order_acquire) == 0;
  bool ok = true;
  for (std::thread& thread : threads_) {
    if (!thread.joinable()) {
      continue;
    }
    if (all_exited) {
      thread.join();
      continue;
    }
    // state_ is a shared_ptr each worker also holds, so detaching here never
    // leaves the worker touching WorkerPool memory after `this` is destroyed
    // — only the (intentionally leaked) SharedState block outlives it.
    log().error << "WorkerPool::Shutdown: worker still live after " << join_budget.count()
                << "ms — detaching (process exit must follow)";
    thread.detach();
    ok = false;
  }
  threads_.clear();
  return ok;
}

size_t WorkerPool::QueuedCount(const WorkerLane lane) const {
  std::lock_guard lock(state_->mutex);
  switch (lane) {
    case WorkerLane::Critical:
      return state_->critical_queue.size();
    case WorkerLane::Normal:
      return state_->normal_queue.size();
    case WorkerLane::Background:
      return state_->background_queue.size();
  }
  return 0;
}

size_t WorkerPool::TotalQueuedCount() const {
  std::lock_guard lock(state_->mutex);
  return state_->critical_queue.size() + state_->normal_queue.size() + state_->background_queue.size();
}

void WorkerPool::WorkerMain(std::shared_ptr<SharedState> state, const size_t worker_index) {
  // CRT/pthread shim — see docs/architecture/PLATFORM_CODE.md (allowlisted in common/).
#if defined(__ANDROID__) || defined(__linux__)
  const std::string name = "pp-worker-" + std::to_string(worker_index);
  pthread_setname_np(pthread_self(), name.c_str());
#else
  (void)worker_index;
#endif

  // A counted task settles under the lock the loop takes next, once its captures are gone.
  bool settle = false;
  uint64_t settle_origin = 0;
  for (;;) {
    QueuedTask task;
    {
      std::unique_lock lock(state->mutex);
      if (settle) {
        SettleLocked(*state, settle_origin);
        settle = false;
      }
      state->cv.wait(lock, [&state]() { return state->stopped || (!state->paused && HasWorkLocked(*state)); });
      // Once Shutdown sets stopped, never dequeue — queued work was dropped under the same lock.
      if (state->stopped) {
        state->live_workers.fetch_sub(1, std::memory_order_acq_rel);
        state->cv.notify_all();
        break;
      }
      if (state->paused || !DequeueOneLocked(*state, &task)) {
        continue;
      }
    }
    running_on_this_thread_ = {state.get(), task.counted, task.origin};
    RunTaskSafely(state->logger, task.run);
    task.run = nullptr; // part of the task: what its captures' destructors post inherits its origin
    running_on_this_thread_ = {};
    settle = task.counted;
    settle_origin = task.origin;
  }
}

bool WorkerPool::DequeueOneLocked(SharedState& state, QueuedTask* out) {
  std::deque<QueuedTask>* queue = nullptr;
  if (!state.critical_queue.empty()) {
    queue = &state.critical_queue;
  } else if (!state.normal_queue.empty()) {
    queue = &state.normal_queue;
  } else if (!state.background_queue.empty()) {
    queue = &state.background_queue;
  } else {
    return false;
  }

  *out = std::move(queue->front());
  queue->pop_front();
  return true; // Post refuses empty tasks; a counted one must reach the settle either way
}

bool WorkerPool::HasWorkLocked(const SharedState& state) {
  return !state.critical_queue.empty() || !state.normal_queue.empty() || !state.background_queue.empty();
}

void WorkerPool::SettleLocked(SharedState& state, const uint64_t origin) {
  const auto it = state.outstanding.find(origin);
  if (it != state.outstanding.end() && --it->second == 0) {
    state.outstanding.erase(it);
  }
  if (state.barrier_waiters > 0) {
    state.settled_cv.notify_all();
  }
}

WorkerPool::CompletionBarrier::CompletionBarrier(WorkerPool& pool)
    : state_(pool.state_), caller_(running_on_this_thread_) {
  {
    std::lock_guard lock(state_->mutex);
    ticket_ = state_->next_ticket;
  }
  running_on_this_thread_ = {}; // posts from this thread while the barrier lives are new work
}

WorkerPool::CompletionBarrier::~CompletionBarrier() {
  running_on_this_thread_ = caller_;
}

bool WorkerPool::CompletionBarrier::Wait(const std::chrono::milliseconds wait) {
  std::unique_lock lock(state_->mutex);
  if (SettledLocked()) {
    return true;
  }
  ++state_->barrier_waiters;
  const bool settled = state_->settled_cv.wait_for(lock, wait, [this]() { return SettledLocked(); });
  --state_->barrier_waiters;
  return settled;
}

bool WorkerPool::CompletionBarrier::SettledLocked() const {
  size_t earlier = 0;
  for (auto it = state_->outstanding.begin(); it != state_->outstanding.end() && it->first < ticket_; ++it) {
    earlier += it->second;
  }
  // The calling task (made on one of this pool's counted tasks) is outstanding and below the ticket, and cannot
  // finish while its thread waits here.
  const size_t caller = caller_.pool == state_.get() && caller_.counted ? 1 : 0;
  return earlier <= caller;
}

void WorkerPool::RunTaskSafely(logging::Logger& logger, std::function<void()>& task) {
  if (!task) {
    return;
  }
  try {
    task();
  } catch (const std::exception& e) {
    logger.error << "Uncaught exception in worker task: " << e.what();
  } catch (...) {
    logger.error << "Uncaught unknown exception in worker task";
  }
}

} // namespace pp
