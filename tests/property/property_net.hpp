// The machine a property test of use() runs the real License on: two processes of the app over one state file and one device
// clock, a KeyGrant server whose every answer a step may lose, delay or refuse, a disk that may refuse its writes, and a
// device clock that jumps forward and back.
//
// Concurrency is deterministic. A request of a process is PARKED at the world, which decides when and how it is answered, and
// a call is started only once every call running before it is parked at a request or done, so one thread moves at a time and
// a case replays the same way on any machine. The races that matter (a process's whole call between another's request and its
// answer) are the order the world answers its parked requests in. A request nobody answers is timed out one at a time, as an
// adapter's is at its bound: no answer, to the SDK; no clock is raced against a timer (the License is given bounds no machine
// reaches).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "keygrant/keygrant.hpp"
#include "property/property_server.hpp"
#include "support.hpp"

namespace kgtest::property {

/// A property that did not hold: which, and where.
class PropertyFailure : public std::runtime_error {
 public:
  PropertyFailure(std::string invariant, const std::string& message)
      : std::runtime_error(invariant + ": " + message), invariant_(std::move(invariant)) {}
  const std::string& invariant() const noexcept { return invariant_; }

 private:
  std::string invariant_;
};

enum class Net { Good, Down, Flaky };
Net netOf(const std::string& word);
const char* name(Net net);

enum class Fate { Ok, Lost, LostAfter, Hang, HangAfter, Fail, FailAfter, Limited };
Fate fateOf(Net net, double r);

/// A request a process made for one of its use() calls.
struct Req {
  std::string meter;
  std::int64_t n = 0;
  std::string id;
  /// The server recorded the spend.
  bool applied = false;
  /// The SDK got a usable answer.
  bool answered = false;
};

/// What a parked request is given: an answer, or none (the adapter throws, as a lost connection does).
class Slot {
 public:
  void respond(keygrant::HttpResponse response);
  void fail();
  /// Blocks until given; throws when it was given no answer.
  keygrant::HttpResponse wait();

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool given_ = false;
  bool failed_ = false;
  keygrant::HttpResponse response_;
};

/// A request parked at the world: it is answered, lost or left to time out when the world says.
struct Parked {
  int index = 0;
  std::string action;
  json body;
  Fate fate = Fate::Ok;
  bool hung = false;
  std::shared_ptr<Slot> slot;
};

/// What a call came to: its result, or the text of what it threw.
struct CallOutcome {
  std::optional<std::string> error;
  std::optional<keygrant::UseResult> use;
  std::optional<keygrant::LicenseStatus> status;
  std::optional<keygrant::DeactivateResult> release;
  std::optional<keygrant::ActivateResult> activate;
};

/// A call under way on a thread of its own: the process it is made on, and whether it is done.
struct Running {
  int p = 0;
  std::atomic<bool> done{false};
  CallOutcome outcome;
  std::thread thread;
};

class Shared;

/// Everything the adapters and the world share, guarded by Shared::mutex.
struct Inner {
  Inner(StoredState initial, PropServer initialServer) : state(std::move(initial)), server(std::move(initialServer)) {}

  StoredState state;
  PropServer server;
  Net net = Net::Good;
  std::vector<double> rs{0.5};
  std::size_t rk = 0;
  std::optional<Parked> parked[2];
  std::vector<std::shared_ptr<Running>> active;
  std::vector<std::shared_ptr<keygrant::License>> procs;
  std::vector<Req> useReqs[2];
  /// The releases a step made: whether the server recorded each, and the entries it carried.
  std::vector<std::pair<bool, std::set<std::string>>> releases;
  /// The ids of spends a request reported as plain spends (not as voids): spends the customer was told were made.
  std::set<std::string> reportedPlain;
  /// The voids the contract let be lost before the server heard them.
  std::set<std::string> lostVoids;
  /// The voids held in memory when a save left the journal at its size: one that did not land may have found no room.
  std::set<std::string> refusedVoids;
  /// A save that dropped a void with nothing to replace it, found inside the SDK's save and reported by the next check.
  std::optional<std::string> violation;
  bool tracing = false;
  std::vector<std::string> trace;
  std::map<std::string, std::string> labels;
  std::set<std::string> preIds;
  bool closing = false;
};

class Shared : public std::enable_shared_from_this<Shared> {
 public:
  Shared(StoredState state, PropServer server, TestKey key, std::int64_t now);

  std::mutex mutex;
  std::condition_variable changed;
  Inner inner;
  TestKey key;
  std::atomic<std::int64_t> trueNow;
  std::atomic<std::int64_t> offset{0};
  std::atomic<bool> diskFailing{false};

  /// The device clock.
  std::int64_t clockNow() const { return trueNow.load() + offset.load(); }

  /// Something changed that the world may be waiting for.
  void signal();

  /// A process of the app: the real License, over the world's adapters.
  std::shared_ptr<keygrant::License> process(int index);

  /// Start a call on process `p` on a thread of its own.
  std::shared_ptr<Running> launch(int p, std::function<void(keygrant::License&, CallOutcome&)> work);

  /// Wait for the calls just started to park at a request or be done.
  void settle(std::size_t step);

  /// Answer the requests parked, one at a time in an order the case fixes, until every call started is done.
  void drive(std::size_t step);

  /// The world is over: whatever is parked is given no answer, and every call is waited for.
  void shutdown();

 private:
  bool quiescent() const;
  std::string whereabouts() const;
  void release(Parked request);
  void until(const std::string& what, const std::function<bool()>& condition);
};

/// An id as the trace shows it: the order it was first seen in, so a case reads the same every time. Under the lock.
std::string label(Inner& inner, const std::string& id);

}  // namespace kgtest::property
