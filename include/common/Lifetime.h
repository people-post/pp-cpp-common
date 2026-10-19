#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace pp {

/**
 * Keeps work posted elsewhere — a queue, a timer, a network callback — from reaching an object that
 * stopped or went away. The object binds what it posts (Bind, Post); Invalidate makes everything bound
 * so far a no-op, and work bound afterwards runs again (an object that aborts mid-life and goes on).
 *
 * - **Same thread:** an owner that invalidates on the thread its bound work runs on needs nothing
 *   more — no bound call can be running then.
 * - **Another thread:** InvalidateAndWait also waits for bound calls in progress, so once it returns
 *   none runs (a guest on a host's io thread, stopped from its own). Called from inside a bound call,
 *   it waits for the others only.
 *
 * Lock-free on the call path. Code that checks by hand (`Alive(token, snapshot)`) is not counted as in
 * progress: use it on the owner's thread only.
 */
class Lifetime {
public:
  struct State {
    std::atomic<uint64_t> generation{0};
    /** Bound calls running now, on any thread. */
    std::atomic<uint32_t> inside{0};
  };
  using Token = std::shared_ptr<State>;

  Lifetime() : state_(std::make_shared<State>()) {}
  /** The owner is going: what it bound becomes a no-op (it does not wait; see InvalidateAndWait). */
  ~Lifetime() { Invalidate(); }

  Lifetime(const Lifetime&) = delete;
  Lifetime& operator=(const Lifetime&) = delete;

  Token token() const { return state_; }
  uint64_t Snapshot() const { return state_->generation.load(std::memory_order_seq_cst); }

  static bool Alive(const Token& token, uint64_t snapshot) {
    return token && token->generation.load(std::memory_order_seq_cst) == snapshot;
  }
  bool Alive(uint64_t snapshot) const { return Alive(state_, snapshot); }

  /** Everything bound so far becomes a no-op; a bound call already running finishes. */
  void Invalidate() { state_->generation.fetch_add(1, std::memory_order_seq_cst); }

  /** Invalidate, then wait until no bound call runs on another thread. */
  void InvalidateAndWait() {
    Invalidate();
    const uint32_t mine = InsideOnThisThread(state_.get());
    for (uint32_t now = state_->inside.load(std::memory_order_seq_cst); now > mine;
         now = state_->inside.load(std::memory_order_seq_cst)) {
      state_->inside.wait(now, std::memory_order_seq_cst);
    }
  }

  /** `fn`, run only while the generation it was bound in lasts; counted while it runs. */
  template <typename F> auto Bind(F&& fn) const {
    Token token = state_;
    const uint64_t snapshot = Snapshot();
    // Shared so the result is const-callable (std::function) and may wrap a mutable functor.
    auto held = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
    return [token, snapshot, held](auto&&... args) {
      // Counted before the check: InvalidateAndWait sees the call, or the call sees the bump.
      token->inside.fetch_add(1, std::memory_order_seq_cst);
      const Inside inside(token.get());
      if (token->generation.load(std::memory_order_seq_cst) != snapshot) {
        return;
      }
      std::invoke(*held, std::forward<decltype(args)>(args)...);
    };
  }

  /** Bind `task`, then hand it to `post`. */
  template <typename PostFn> void Post(PostFn&& post, std::function<void()> task) const {
    if (!task) {
      return;
    }
    std::forward<PostFn>(post)(Bind(std::move(task)));
  }

private:
  /** Leaves a bound call: uncounts it (also when it throws) and wakes a waiting invalidation. */
  class Inside {
  public:
    explicit Inside(State* state) : state_(state) { Entered().push_back(state); }
    ~Inside() {
      auto& entered = Entered();
      for (auto it = entered.rbegin(); it != entered.rend(); ++it) {
        if (*it == state_) {
          entered.erase(std::next(it).base());
          break;
        }
      }
      state_->inside.fetch_sub(1, std::memory_order_seq_cst);
      state_->inside.notify_all();
    }
    Inside(const Inside&) = delete;
    Inside& operator=(const Inside&) = delete;

  private:
    State* state_;
  };

  /** The bound calls this thread is inside, innermost last. */
  static std::vector<const State*>& Entered() {
    thread_local std::vector<const State*> entered;
    return entered;
  }
  static uint32_t InsideOnThisThread(const State* state) {
    uint32_t count = 0;
    for (const State* entered : Entered()) {
      count += entered == state ? 1 : 0;
    }
    return count;
  }

  Token state_;
};

} // namespace pp
