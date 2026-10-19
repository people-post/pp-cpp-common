#include "common/SerialExecutor.h"

#include <condition_variable>
#include <memory>
#include <mutex>

namespace pp {
namespace {

/** Where a RunAndWait caller waits: the task ran here, or the executor dropped it. */
class Rendezvous {
public:
  enum class Outcome { Waiting, Ran, Dropped };

  void Settle(Outcome outcome) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (outcome_ != Outcome::Waiting) {
        return;
      }
      outcome_ = outcome;
    }
    settled_.notify_all();
  }

  Outcome Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    settled_.wait(lock, [this]() { return outcome_ != Outcome::Waiting; });
    return outcome_;
  }

private:
  std::mutex mutex_;
  std::condition_variable settled_;
  Outcome outcome_{Outcome::Waiting};
};

/**
 * Travels inside the posted task: settles Ran once the task returned (also when it threw), and
 * Dropped when the executor destroys the task without running it.
 */
class Ticket {
public:
  explicit Ticket(std::shared_ptr<Rendezvous> rendezvous) : rendezvous_(std::move(rendezvous)) {}
  ~Ticket() { rendezvous_->Settle(ran_ ? Rendezvous::Outcome::Ran : Rendezvous::Outcome::Dropped); }
  Ticket(const Ticket&) = delete;
  Ticket& operator=(const Ticket&) = delete;

  void Run(const Executor::Task& task) {
    struct Settle {
      Ticket& ticket;
      ~Settle() { ticket.rendezvous_->Settle(Rendezvous::Outcome::Ran); }
    } settle{*this};
    ran_ = true;
    task();
  }

private:
  std::shared_ptr<Rendezvous> rendezvous_;
  bool ran_{false};
};

} // namespace

void SerialExecutor::RunAndWait(const Task& task) {
  if (!task) {
    return;
  }
  if (RunsTasksOnCurrentThread()) {
    task();
    return;
  }
  auto rendezvous = std::make_shared<Rendezvous>();
  {
    // The caller waits until the task ran or was dropped, so `task` outlives every use of it.
    auto ticket = std::make_shared<Ticket>(rendezvous);
    Post([ticket, &task]() { ticket->Run(task); });
  }
  if (rendezvous->Wait() == Rendezvous::Outcome::Dropped) {
    task();
  }
}

} // namespace pp
