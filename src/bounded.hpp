// A call held to a time bound whatever it does: the reference's withTimeout.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace keygrant::detail {

/// The longest any one bound is held to (a wait any longer could overflow a clock's arithmetic).
inline constexpr std::chrono::milliseconds kLongestBound{24LL * 60 * 60 * 1000};

/**
 * `work`'s value, or none when it threw or did not finish within `bound`.
 *
 * The work runs on a thread of its own, detached: one that outlives its bound
 * is abandoned, left to finish on its own, and its value dropped. That is how
 * an adapter that ignores its timeout cannot hold a call open, as the
 * reference's promise race abandons a fetch. Whatever the work captures (by
 * shared_ptr, by value) lives as long as it runs. No thread at all (none to be
 * had) is no answer.
 */
template <class T>
std::optional<T> runBounded(std::function<T()> work, std::chrono::milliseconds bound) {
  struct Shared {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::optional<T> value;
  };
  auto shared = std::make_shared<Shared>();
  try {
    std::thread([shared, work = std::move(work)]() mutable {
      std::optional<T> value;
      try {
        value.emplace(work());
      } catch (...) {
        // A failure is no answer, as a timeout is.
      }
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->value = std::move(value);
      shared->finished = true;
      shared->done.notify_all();
    }).detach();
  } catch (...) {
    return std::nullopt;
  }
  const auto wait = std::clamp(bound, std::chrono::milliseconds{0}, kLongestBound);
  std::unique_lock<std::mutex> lock(shared->mutex);
  if (!shared->done.wait_for(lock, wait, [&] { return shared->finished; })) return std::nullopt;
  return std::move(shared->value);
}

/**
 * A call running on a thread of its own that any number of callers wait on,
 * each for at most its own bound: how a read that may never end (a hung
 * filesystem, a filter driver, an adapter that ignores its timeout) is made
 * once and joined by every call that comes while it runs, rather than started
 * again beside it, a thread lost each time. Abandoned, left to finish, by every
 * caller it outlives; what it gives is none when it threw.
 */
template <class T>
class SharedCall {
 public:
  /// `work` started on a thread of its own; a call that could not be started is finished, with nothing.
  static std::shared_ptr<SharedCall> start(std::function<T()> work) {
    auto call = std::make_shared<SharedCall>();
    try {
      std::thread([call, work = std::move(work)]() mutable {
        std::optional<T> value;
        try {
          value.emplace(work());
        } catch (...) {
          // A failure is no answer, as a timeout is.
        }
        {
          std::lock_guard<std::mutex> lock(call->mutex_);
          call->value_ = std::move(value);
          call->finished_ = true;
        }
        call->done_.notify_all();
      }).detach();
    } catch (...) {
      std::lock_guard<std::mutex> lock(call->mutex_);
      call->finished_ = true;
    }
    return call;
  }

  /** What it gave, waiting at most `bound`; none when it threw or is still running then. */
  std::optional<T> wait(std::chrono::milliseconds bound) {
    const auto limit = std::clamp(bound, std::chrono::milliseconds{0}, kLongestBound);
    std::unique_lock<std::mutex> lock(mutex_);
    if (!done_.wait_for(lock, limit, [this] { return finished_; })) return std::nullopt;
    return value_;
  }

  /** Whether it has finished: a caller that comes after it starts another. */
  bool finished() {
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable done_;
  bool finished_ = false;
  std::optional<T> value_;
};

}  // namespace keygrant::detail
