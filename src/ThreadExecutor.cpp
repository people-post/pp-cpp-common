#include "common/ThreadExecutor.h"

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

void ThreadExecutor::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    tasks_.clear();
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
      wake_.wait(lock, [this]() { return stopped_ || !tasks_.empty(); });
      if (stopped_) {
        return;
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
