#include "property/property_net.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "engine.hpp"

namespace kgtest::property {

using keygrant::SpendEntry;
using keygrant::SpendKind;
using namespace std::chrono_literals;

constexpr std::size_t kEntries = 64;
constexpr std::size_t kVoids = 16;

Net netOf(const std::string& word) {
  if (word == "good") return Net::Good;
  if (word == "down") return Net::Down;
  return Net::Flaky;
}

const char* name(Net net) {
  switch (net) {
    case Net::Good: return "good";
    case Net::Down: return "down";
    case Net::Flaky: return "flaky";
  }
  return "good";
}

struct Threshold {
  double below;
  Fate fate;
};

/** The fate of the first threshold `r` is below; the last fate when it is below none. */
Fate firstBelow(std::initializer_list<Threshold> thresholds, double r) {
  for (const Threshold& t : thresholds) {
    if (r < t.below) return t.fate;
  }
  return Fate::HangAfter;
}

Fate fateOf(Net net, double r) {
  switch (net) {
    case Net::Good: return Fate::Ok;
    case Net::Down: return firstBelow({{0.45, Fate::Lost}, {0.75, Fate::LostAfter}, {0.88, Fate::Hang}}, r);
    case Net::Flaky:
      return firstBelow({{0.35, Fate::Ok},       {0.45, Fate::Lost},  {0.55, Fate::LostAfter}, {0.65, Fate::Fail},
                         {0.75, Fate::FailAfter}, {0.82, Fate::Limited}, {0.9, Fate::Hang}},
                        r);
  }
  return Fate::HangAfter;
}

void Slot::respond(keygrant::HttpResponse response) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    given_ = true;
    response_ = std::move(response);
  }
  changed_.notify_all();
}

void Slot::fail() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    given_ = true;
    failed_ = true;
  }
  changed_.notify_all();
}

keygrant::HttpResponse Slot::wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  changed_.wait(lock, [this] { return given_; });
  if (failed_) throw std::runtime_error("network down");
  return response_;
}

std::string label(Inner& inner, const std::string& id) {
  if (id.empty()) return std::string();
  const auto known = inner.labels.find(id);
  if (known != inner.labels.end()) return known->second;
  const std::string shown = inner.preIds.count(id) != 0 ? id.substr(0, 6) : "s" + std::to_string(inner.labels.size());
  inner.labels[id] = shown;
  return shown;
}

namespace {

/// A request noted: traced, and what the world reads of it afterwards.
void noted(Inner& inner, const Parked& req, const Served* served, bool answered) {
  const json& body = req.body;
  if (inner.tracing) {
    std::string reports;
    if (body.contains("spends") && body["spends"].is_array()) {
      const char* separator = "";
      for (const json& e : body["spends"]) {
        reports += separator + label(inner, e.value("id", std::string())) + ":" + e.value("meter", std::string()) + ":" +
                   std::to_string(e.value("n", std::int64_t{0})) + (e.contains("void") ? "v" : "");
        separator = ",";
      }
    }
    const std::string id = label(inner, body.value("id", std::string()));
    inner.trace.push_back("  p" + std::to_string(req.index) + " " + req.action + " {meter:" + body.value("meter", std::string()) +
                          ",n:" + (body.contains("n") ? body["n"].dump() : std::string("null")) + ",id:" + id + ",spends:[" + reports +
                          "]} -> " + (served != nullptr ? std::to_string(served->status) : std::string("lost")) + " " +
                          (answered ? "answered" : "unanswered"));
  }
  if (body.contains("spends") && body["spends"].is_array()) {
    for (const json& e : body["spends"]) {
      if (!e.contains("void") && e.contains("id") && e["id"].is_string()) inner.reportedPlain.insert(e["id"].get<std::string>());
    }
  }
  if (req.action == "deactivate") {
    std::set<std::string> ids;
    if (body.contains("spends") && body["spends"].is_array()) {
      for (const json& e : body["spends"]) {
        if (e.contains("id") && e["id"].is_string()) ids.insert(e["id"].get<std::string>());
      }
    }
    inner.releases.emplace_back(served != nullptr, std::move(ids));
  }
  if (req.action == "use") {
    inner.useReqs[req.index].push_back(Req{body.value("meter", std::string()), body.value("n", std::int64_t{0}),
                                           body.value("id", std::string()), served != nullptr && served->spent.has_value(), answered});
  }
}

/// A save that landed. A void that left the journal and that the server did not confirm was replaced, and a replacement
/// happens only when a void lands with sixteen standing: for each void that lands (one now in the journal that was not, or
/// one held that is in neither, whether refused or confirmed in the same save) the oldest may go. A void dropped otherwise
/// is a fault, kept for the next check to report. A seat taken or dropped is the release's and the restart's to account for.
void landed(Inner& inner, const StoredState& after) {
  const StoredState& before = inner.state;
  if (before.activationId != after.activationId || !after.key) return;
  const std::vector<SpendEntry>& was = before.spends;
  const std::vector<SpendEntry>& now = after.spends;
  std::set<std::string> kept;
  for (const SpendEntry& e : now) kept.insert(e.id);
  std::set<std::string> had;
  for (const SpendEntry& e : was) had.insert(e.id);
  std::vector<std::string> dropped;
  for (const SpendEntry& e : was) {
    if (e.kind == SpendKind::Void && kept.count(e.id) == 0 && inner.server.voided.count(e.id) == 0) dropped.push_back(e.id);
  }
  std::vector<std::string> heldVoids;
  for (const std::shared_ptr<keygrant::License>& proc : inner.procs) {
    for (const keygrant::detail::JournalOp& op : keygrant::detail::LicenseAccess::heldChanges(*proc)) {
      if (op.isVoid()) heldVoids.push_back(op.id);
    }
  }
  std::set<std::string> landing;
  for (const SpendEntry& e : now) {
    if (e.kind == SpendKind::Void && had.count(e.id) == 0) landing.insert(e.id);
  }
  for (const std::string& id : heldVoids) {
    if (had.count(id) == 0 && kept.count(id) == 0) landing.insert(id);
  }
  const std::size_t standing = static_cast<std::size_t>(
      std::count_if(was.begin(), was.end(), [](const SpendEntry& e) { return e.kind == SpendKind::Void; }));
  const std::size_t nowVoids = static_cast<std::size_t>(
      std::count_if(now.begin(), now.end(), [](const SpendEntry& e) { return e.kind == SpendKind::Void; }));
  const bool full = now.size() - (nowVoids >= kVoids ? 1 : 0) >= kEntries;
  if (!dropped.empty()) {
    if (dropped.size() <= landing.size() && standing + landing.size() > kVoids) {
      inner.lostVoids.insert(dropped.begin(), dropped.end());
    } else if (!inner.violation) {
      inner.violation = std::to_string(dropped.size()) + " void(s) left the journal of " + std::to_string(standing) + " voids with " +
                        std::to_string(landing.size()) + " landing, and nothing was to replace them";
    }
  }
  // Left at its size, with sixteen voids' oldest not yet replaced, the journal took no more: a void that was held and did not
  // land found no room.
  if (full) {
    for (const std::string& id : heldVoids) {
      if (kept.count(id) == 0) inner.refusedVoids.insert(id);
    }
  }
}

// --- the adapters -------------------------------------------------------------------------------------------------

class WorldClock final : public keygrant::Clock {
 public:
  explicit WorldClock(std::weak_ptr<Shared> shared) : shared_(std::move(shared)) {}
  std::int64_t now() override {
    const std::shared_ptr<Shared> shared = shared_.lock();
    return shared ? shared->clockNow() : 0;
  }

 private:
  std::weak_ptr<Shared> shared_;
};

class WorldStorage final : public keygrant::StorageAdapter {
 public:
  explicit WorldStorage(std::weak_ptr<Shared> shared) : shared_(std::move(shared)) {}

  std::optional<StoredState> read(bool) override {
    const std::shared_ptr<Shared> shared = lock();
    std::lock_guard<std::mutex> guard(shared->mutex);
    return shared->inner.state;
  }

  void write(const StoredState& state) override {
    const std::shared_ptr<Shared> shared = lock();
    std::lock_guard<std::mutex> guard(shared->mutex);
    if (shared->diskFailing) throw keygrant::IoError("EBUSY", "the disk said no");
    landed(shared->inner, state);
    shared->inner.state = state;
  }

  bool update(const std::function<StoredState(const StoredState&)>& merge) override {
    const std::shared_ptr<Shared> shared = lock();
    std::lock_guard<std::mutex> guard(shared->mutex);
    const StoredState next = merge(shared->inner.state);
    if (shared->diskFailing) throw keygrant::IoError("EBUSY", "the disk said no");
    landed(shared->inner, next);
    shared->inner.state = next;
    return true;
  }

 private:
  std::shared_ptr<Shared> lock() const {
    std::shared_ptr<Shared> shared = shared_.lock();
    if (!shared) throw keygrant::IoError("EIO", "the world is gone");
    return shared;
  }

  std::weak_ptr<Shared> shared_;
};

class WorldHttp final : public keygrant::HttpAdapter {
 public:
  WorldHttp(std::weak_ptr<Shared> shared, int index) : shared_(std::move(shared)), index_(index) {}

  keygrant::HttpResponse post(const std::string& url, const std::string& body, std::chrono::milliseconds) override {
    const std::shared_ptr<Shared> shared = shared_.lock();
    if (!shared) throw std::runtime_error("the world is gone");
    const std::string action = url.substr(url.rfind('/') + 1);
    auto slot = std::make_shared<Slot>();
    {
      std::lock_guard<std::mutex> guard(shared->mutex);
      Inner& inner = shared->inner;
      if (inner.closing) throw std::runtime_error("the world is over");
      const double r = inner.rs.empty() ? 0.5 : inner.rs[inner.rk % inner.rs.size()];
      ++inner.rk;
      inner.parked[index_] = Parked{index_, action, json::parse(body), fateOf(inner.net, r), false, slot};
    }
    shared->changed.notify_all();
    return slot->wait();
  }

 private:
  std::weak_ptr<Shared> shared_;
  int index_;
};

}  // namespace

Shared::Shared(StoredState state, PropServer server, TestKey signing, std::int64_t now)
    : inner(std::move(state), std::move(server)), key(std::move(signing)), trueNow(now) {}

void Shared::signal() {
  { std::lock_guard<std::mutex> guard(mutex); }
  changed.notify_all();
}

std::shared_ptr<keygrant::License> Shared::process(int index) {
  keygrant::LicenseConfig config;
  config.product = PRODUCT;
  config.apiBaseUrl = "https://api.test/";
  config.publicJwk = key.publicJwk();
  // Bounds no machine reaches: a request is answered or timed out by the world, never by a clock.
  config.httpTimeout = std::chrono::minutes(10);
  const std::weak_ptr<Shared> weak = weak_from_this();
  config.adapters.storage = std::make_shared<WorldStorage>(weak);
  config.adapters.clock = std::make_shared<WorldClock>(weak);
  config.adapters.http = std::make_shared<WorldHttp>(weak, index);
  config.adapters.fingerprint = std::make_shared<FakeFingerprinter>();
  config.adapters.registry = std::make_shared<MemoryRegistry>();
  keygrant::detail::Timings timings;
  timings.lockGrace = std::chrono::minutes(10);
  timings.statusCheck = std::chrono::minutes(10);
  return std::make_shared<keygrant::License>(config, timings);
}

std::shared_ptr<Running> Shared::launch(int p, std::function<void(keygrant::License&, CallOutcome&)> work) {
  auto running = std::make_shared<Running>();
  running->p = p;
  std::shared_ptr<keygrant::License> license;
  {
    std::lock_guard<std::mutex> guard(mutex);
    inner.active.push_back(running);
    license = inner.procs[static_cast<std::size_t>(p)];
  }
  // The world joins every call before it ends, so the thread needs no hold on the world.
  running->thread = std::thread([this, running, license, work = std::move(work)] {
    try {
      work(*license, running->outcome);
    } catch (const std::exception& e) {
      running->outcome.error = e.what();
    } catch (...) {
      running->outcome.error = "something that is no exception";
    }
    running->done = true;
    signal();
  });
  return running;
}

bool Shared::quiescent() const {
  for (int p = 0; p < 2; ++p) {
    // The first call of a process not done runs (the others wait their turn); it is moving unless it is parked.
    const bool running = std::any_of(inner.active.begin(), inner.active.end(),
                                     [p](const std::shared_ptr<Running>& a) { return a->p == p && !a->done; });
    if (running && !inner.parked[p].has_value()) return false;
  }
  return true;
}

std::string Shared::whereabouts() const {
  std::string out;
  for (std::size_t i = 0; i < inner.active.size(); ++i) {
    out += "call " + std::to_string(i) + " on p" + std::to_string(inner.active[i]->p) + ": " + (inner.active[i]->done ? "done" : "running") + "; ";
  }
  for (int p = 0; p < 2; ++p) {
    const std::optional<Parked>& parked = inner.parked[p];
    out += "p" + std::to_string(p) + ": " + (parked ? parked->action + (parked->hung ? " hung" : "") : std::string("nothing parked")) + "; ";
  }
  return out;
}

void Shared::until(const std::string& what, const std::function<bool()>& condition) {
  const auto began = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(mutex);
  while (!condition()) {
    if (std::chrono::steady_clock::now() - began > 30s) {
      throw PropertyFailure("stuck", "a call never settled (" + what + "): " + whereabouts());
    }
    changed.wait_for(lock, 50ms);
  }
}

void Shared::settle(std::size_t step) { until("step " + std::to_string(step), [this] { return quiescent(); }); }

namespace {

keygrant::HttpResponse responseOf(int status, const json& body) { return keygrant::HttpResponse{status, body.dump()}; }

}  // namespace

/// What a request comes to over a network that may lose it, or its answer, or both. Under the lock.
void Shared::release(Parked req) {
  const auto answer = [this, &req](bool answered) {
    const Served served = inner.server.handle(req.action, req.body);
    noted(inner, req, &served, answered);
    return served;
  };
  const std::size_t index = static_cast<std::size_t>(req.index);
  switch (req.fate) {
    case Fate::Ok: {
      const Served served = answer(true);
      req.slot->respond(responseOf(served.status, served.body));
      break;
    }
    case Fate::LostAfter:
      answer(false);
      req.slot->fail();
      break;
    case Fate::HangAfter:
      answer(false);
      req.hung = true;
      inner.parked[index] = std::move(req);
      break;
    case Fate::FailAfter: {
      const Served served = answer(false);
      req.slot->respond(responseOf(500, served.body));
      break;
    }
    case Fate::Lost:
      noted(inner, req, nullptr, false);
      req.slot->fail();
      break;
    case Fate::Hang:
      noted(inner, req, nullptr, false);
      req.hung = true;
      inner.parked[index] = std::move(req);
      break;
    case Fate::Fail:
      noted(inner, req, nullptr, false);
      req.slot->respond(responseOf(503, {{"error", "unavailable"}}));
      break;
    case Fate::Limited:
      noted(inner, req, nullptr, false);
      req.slot->respond(responseOf(429, {{"error", "rate-limited"}}));
      break;
  }
}

void Shared::drive(std::size_t step) {
  for (int rounds = 0;; ++rounds) {
    settle(step);
    {
      std::lock_guard<std::mutex> lock(mutex);
      const bool allDone = std::all_of(inner.active.begin(), inner.active.end(), [](const std::shared_ptr<Running>& a) { return a->done.load(); });
      if (allDone) break;
      if (rounds > 400) throw PropertyFailure("stuck", "a call never settled (step " + std::to_string(step) + ")");
      const double last = inner.rs.empty() ? 0.5 : inner.rs.back();
      // Two requests parked: the case says whose is answered first.
      std::vector<int> waiting;
      for (int i = 0; i < 2; ++i) {
        if (inner.parked[i] && !inner.parked[i]->hung) waiting.push_back(i);
      }
      if (!waiting.empty()) {
        const int pick = waiting.size() == 1 || last < 0.5 ? waiting.front() : waiting.back();
        Parked req = std::move(*inner.parked[pick]);
        inner.parked[pick].reset();
        release(std::move(req));
        continue;
      }
      // Two left that nobody answers: one at a time, in the order the case fixes, so a case replays the same way.
      std::vector<int> hung;
      for (int i = 0; i < 2; ++i) {
        if (inner.parked[i]) hung.push_back(i);
      }
      if (hung.empty()) throw PropertyFailure("stuck", "a call never settled (step " + std::to_string(step) + "): " + whereabouts());
      const int pick = hung.size() == 1 || last < 0.5 ? hung.front() : hung.back();
      // A request nobody answers times out, as an adapter's does at its bound (no answer, to the SDK).
      const std::shared_ptr<Slot> silent = inner.parked[pick]->slot;
      inner.parked[pick].reset();
      silent->fail();
    }
  }
  std::vector<std::shared_ptr<Running>> finished;
  {
    std::lock_guard<std::mutex> lock(mutex);
    finished.swap(inner.active);
  }
  for (const std::shared_ptr<Running>& call : finished) {
    if (call->thread.joinable()) call->thread.join();
  }
}

void Shared::shutdown() {
  std::vector<std::shared_ptr<Running>> calls;
  std::string where;
  {
    std::lock_guard<std::mutex> lock(mutex);
    inner.closing = true;
    where = whereabouts();
    for (std::optional<Parked>& parked : inner.parked) {
      if (parked) parked->slot->fail();
      parked.reset();
    }
    calls = inner.active;
    inner.active.clear();
  }
  // A call that was waiting for the next request finds the world over, and ends.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  for (const std::shared_ptr<Running>& call : calls) {
    while (!call->done) {
      if (std::chrono::steady_clock::now() > deadline) {
        // A call blocked on something other than a request of the world: ending the run beats a CI job that never ends.
        std::fprintf(stderr, "the property world could not end: a call did not finish within 30 seconds of the world closing (%s)\n",
                     where.c_str());
        std::abort();
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::optional<Parked>& parked : inner.parked) {
          if (parked) parked->slot->fail();
          parked.reset();
        }
      }
      std::this_thread::sleep_for(1ms);
    }
    if (call->thread.joinable()) call->thread.join();
  }
  std::lock_guard<std::mutex> lock(mutex);
  inner.procs.clear();
}

}  // namespace kgtest::property
