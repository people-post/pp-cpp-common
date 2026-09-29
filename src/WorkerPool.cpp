#include "common/WorkerPool.h"

#include <algorithm>
#include <exception>
#include <string>

#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#endif

namespace pp {

namespace {

std::deque<std::function<void()>>* QueueForLane(WorkerLane lane,
                                                std::deque<std::function<void()>>* critical,
                                                std::deque<std::function<void()>>* normal,
                                                std::deque<std::function<void()>>* background) {
  switch (lane) {
  case WorkerLane::Critical:
    return critical;
  case WorkerLane::Normal:
    return normal;
  case WorkerLane::Background:
    return background;
  }
  return normal;
}

} // namespace

size_t WorkerPool::ClampThreadCount(const size_t thread_count) {
  if (thread_count < kMinThreadCount) {
    return kMinThreadCount;
  }
  return std::min(thread_count, kMaxThreadCount);
}

WorkerPool::WorkerPool(const size_t thread_count)
    : thread_count_(ClampThreadCount(thread_count)),
      state_(std::make_shared<SharedState>(log())) {
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
    EnqueueLocked(*state_, lane, std::move(task));
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
    state_->critical_queue.clear();
    state_->normal_queue.clear();
    state_->background_queue.clear();
  }
  state_->cv.notify_all();

  const auto deadline = std::chrono::steady_clock::now() + join_budget;
  {
    std::unique_lock lock(state_->mutex);
    state_->cv.wait_until(lock, deadline, [this]() {
      return state_->live_workers.load(std::memory_order_acquire) == 0;
    });
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
  return state_->critical_queue.size() + state_->normal_queue.size() +
         state_->background_queue.size();
}

void WorkerPool::WorkerMain(std::shared_ptr<SharedState> state, const size_t worker_index) {
  // CRT/pthread shim — see docs/architecture/PLATFORM_CODE.md (allowlisted in common/).
#if defined(__ANDROID__) || defined(__linux__)
  const std::string name = "pp-worker-" + std::to_string(worker_index);
  pthread_setname_np(pthread_self(), name.c_str());
#else
  (void)worker_index;
#endif

  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(state->mutex);
      state->cv.wait(lock, [&state]() {
        return state->stopped || (!state->paused && HasWorkLocked(*state));
      });
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
    RunTaskSafely(state->logger, task);
  }
}

bool WorkerPool::DequeueOneLocked(SharedState& state, std::function<void()>* out) {
  std::deque<std::function<void()>>* queue = nullptr;
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
  return static_cast<bool>(*out);
}

bool WorkerPool::HasWorkLocked(const SharedState& state) {
  return !state.critical_queue.empty() || !state.normal_queue.empty() ||
         !state.background_queue.empty();
}

void WorkerPool::EnqueueLocked(SharedState& state, WorkerLane lane, std::function<void()> task) {
  std::deque<std::function<void()>>* const queue =
      QueueForLane(lane, &state.critical_queue, &state.normal_queue, &state.background_queue);
  queue->push_back(std::move(task));
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
