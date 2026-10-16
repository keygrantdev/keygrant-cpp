#include "property/property_world.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "engine.hpp"

namespace kgtest::property {

using keygrant::LicenseStatus;
using keygrant::SpendEntry;
using keygrant::SpendKind;
using keygrant::UseError;
using keygrant::UseResult;
using keygrant::detail::JournalOp;

constexpr std::int64_t kDay = 24 * kHour;
constexpr std::int64_t kMinuteMs = kMinute;

/// The contract's numbers: the journal's entries and voids.
constexpr std::size_t kEntries = 64;
constexpr std::size_t kVoids = 16;

namespace {

// --- the oracle's own reading of the contract -----------------------------------------------------------------------------

/// The unsynced spends of a meter, its `offline` and `unanswered` entries, never a void.
std::int64_t unsyncedIn(const std::vector<SpendEntry>& spends, const std::string& meter) {
  std::int64_t sum = 0;
  for (const SpendEntry& e : spends) {
    if (e.meter == meter && e.kind != SpendKind::Void) sum += e.n;
  }
  return sum;
}

/// The payload of a lease, read here and not by the SDK: base64url, then JSON.
json payloadOf(const std::string& lease) {
  const std::size_t first = lease.find('.');
  if (first == std::string::npos) return json::object();
  const std::size_t second = lease.find('.', first + 1);
  const std::string text = lease.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
  std::string bytes;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : text) {
    int sextet = 63;
    if (c >= 'A' && c <= 'Z') {
      sextet = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
      sextet = c - 'a' + 26;
    } else if (c >= '0' && c <= '9') {
      sextet = c - '0' + 52;
    } else if (c == '-') {
      sextet = 62;
    }
    buffer = (buffer << 6) | static_cast<std::uint32_t>(sextet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<char>((buffer >> bits) & 0xff));
    }
  }
  const json parsed = json::parse(bytes, nullptr, false);
  return parsed.is_discarded() ? json::object() : parsed;
}

/// A meter's base figure (left, offline): the stored one when its time is at or after the lease's second, else the lease's own.
std::optional<std::pair<std::int64_t, std::int64_t>> baseFigure(const StoredState& state, const std::string& meter) {
  if (!state.key || !state.lease) return std::nullopt;
  const json claims = payloadOf(*state.lease);
  const std::int64_t iat = claims.value("iat", std::int64_t{0});
  const auto stored = state.meterFigures.find(meter);
  if (stored != state.meterFigures.end() && stored->second.at >= iat * 1000) {
    return std::make_pair(stored->second.left, stored->second.offline);
  }
  if (!claims.contains("met") || !claims["met"].contains(meter)) return std::nullopt;
  const json& met = claims["met"][meter];
  return std::make_pair(met.at("left").get<std::int64_t>(), met.at("offline").get<std::int64_t>());
}

/// The journal's wait: k unanswered or void entries (none ahead of `now`), a minute doubling to an hour after the newest.
bool journalWait(const std::vector<SpendEntry>& spends, std::int64_t now) {
  std::size_t count = 0;
  std::int64_t newest = 0;
  for (const SpendEntry& e : spends) {
    if (e.kind == SpendKind::Offline || e.at > now) continue;
    newest = count == 0 ? e.at : std::max(newest, e.at);
    ++count;
  }
  if (count == 0) return false;
  return now < newest + std::min(kHour, kMinuteMs * (std::int64_t{1} << std::min<std::size_t>(count - 1, 6)));
}

/// The wait of a spend the allowance refuses: a void under a minute old, by the guarded clock and the device clock.
bool voidWait(const std::vector<SpendEntry>& spends, std::int64_t now, std::int64_t clockNow) {
  return std::any_of(spends.begin(), spends.end(), [&](const SpendEntry& e) {
    return e.kind == SpendKind::Void && e.at <= now && e.at <= clockNow && now < e.at + kMinuteMs;
  });
}

/// The wait an unanswered ask leaves, by the device clock.
bool askWait(const StoredState& state, std::int64_t clockNow) {
  return state.backoffSince && state.backoffMs == kMinuteMs && clockNow >= *state.backoffSince && clockNow < *state.backoffSince + kMinuteMs;
}

void credit(std::map<std::string, std::int64_t>& into, const std::string& meter, std::int64_t n) {
  const auto found = into.find(meter);
  if (found != into.end()) found->second += n;
}

std::string preId(std::size_t i) {
  char text[32];
  std::snprintf(text, sizeof text, "pre%03zu", i);
  std::string id = text;
  id.resize(22, 'p');
  return id;
}

const TestKey& worldKey() {
  static const TestKey key;
  return key;
}

}  // namespace

World::World(Case c, bool tracing) : case_(std::move(c)) {
  PropServer server(case_.meters, worldKey(), NOW);
  for (const auto& [code, spec] : case_.meters) {
    (void)spec;
    acked_[code] = 0;
    told_[code] = 0;
    lossAllowed_[code] = 0;
  }
  std::vector<SpendEntry> spends;
  std::set<std::string> preIds;
  for (std::size_t i = 0; i < case_.preload.size(); ++i) {
    const Preloaded& e = case_.preload[i];
    spends.push_back(SpendEntry{preId(i), e.meter, e.n, NOW - e.agoMs, e.kind});
    preIds.insert(preId(i));
    if (e.recorded) server.recorded[preId(i)] = Held{e.meter, e.n};
    if (e.kind == SpendKind::Void) {
      phantoms_.push_back(Phantom{preId(i), e.meter, e.n, false});
    } else {
      credit(acked_, e.meter, e.n);
    }
  }
  StoredState state;
  state.key = KEY;
  state.activationId = "act_1";
  state.lastSeen = NOW;
  state.spends = spends;
  // A journal that begins past the cap is one that spends told to the customer once landed past it.
  toldOps_ = spends.size() > kEntries ? spends.size() - kEntries : 0;
  shared_ = std::make_shared<Shared>(std::move(state), std::move(server), worldKey(), NOW);
  const std::string lease = shared_->inner.server.lease("act_1");
  shared_->inner.state.lease = lease;
  shared_->inner.preIds = std::move(preIds);
  shared_->inner.tracing = tracing;
  shared_->inner.procs = {shared_->process(0), shared_->process(1)};
}

World::~World() { shared_->shutdown(); }

std::vector<std::string> World::trace() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->inner.trace;
}

std::shared_ptr<keygrant::License> World::proc(int p) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->inner.procs[static_cast<std::size_t>(p)];
}

std::vector<std::shared_ptr<keygrant::License>> World::procs() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->inner.procs;
}

void World::fail(const std::string& invariant, const std::string& message) {
  const std::string step = stepNo_ < case_.steps.size() ? case_.steps[stepNo_].toJson().dump() : std::string("end");
  throw PropertyFailure(invariant, message + " (step " + std::to_string(stepNo_) + ": " + step + ")");
}

bool World::tracing() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->inner.tracing;
}

void World::note(std::string line) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->inner.trace.push_back(std::move(line));
}

std::string World::lbl(const std::string& id) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return label(shared_->inner, id);
}

StoredState World::current() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->inner.state;
}

std::vector<JournalOp> World::held(int p) { return keygrant::detail::LicenseAccess::heldChanges(*proc(p)); }

std::vector<SpendEntry> World::viewEntries(int p) {
  const std::optional<StoredState> seen = keygrant::detail::LicenseAccess::seenState(*proc(p));
  return seen ? seen->spends : std::vector<SpendEntry>{};
}

// --- the steps ----------------------------------------------------------------------------------------------------------------

void World::run() {
  for (std::size_t i = 0; i < case_.steps.size(); ++i) {
    stepNo_ = i;
    const Step& s = case_.steps[i];
    if (tracing()) {
      json shown = s.toJson();
      shown.erase("r");
      Net net = Net::Good;
      {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        net = shared_->inner.net;
      }
      note("step " + std::to_string(i) + ": " + shown.dump() + " net=" + name(net) + " disk=" +
           (shared_->diskFailing ? "failing" : "ok") + " clock=" + std::to_string(shared_->clockNow() - NOW));
    }
    step(s);
    if (tracing()) describe();
    check();
  }
  stepNo_ = case_.steps.size();
  converge();
}

void World::setDraws(const std::vector<double>& r) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->inner.rs = r.empty() ? std::vector<double>{0.5} : r;
  shared_->inner.rk = 0;
}

void World::step(const Step& s) {
  switch (s.kind) {
    case Step::Kind::Wait: pass(s.ms); return;
    case Step::Kind::Repeat:
      setDraws(s.r);
      for (std::int64_t i = 0; i < s.times; ++i) {
        pass(s.gapMs);
        ensureActive();
        useCalls({s.call}, std::nullopt);
        check();
      }
      return;
    case Step::Kind::Jump:
      shared_->offset = std::clamp<std::int64_t>(shared_->offset.load() + s.ms, -3 * kDay, 3 * kDay);
      return;
    case Step::Kind::Net: {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      shared_->inner.net = netOf(s.mode);
      return;
    }
    case Step::Kind::Disk: shared_->diskFailing = s.failing; return;
    case Step::Kind::Status: {
      setDraws(s.r);
      ensureActive();
      const std::shared_ptr<Running> running = launchStatus(s.p, s.force);
      shared_->settle(stepNo_);
      shared_->drive(stepNo_);
      expectStatus(*running);
      return;
    }
    case Step::Kind::Deactivate:
      setDraws(s.r);
      ensureActive();
      deactivate(s.p);
      return;
    case Step::Kind::Restart: restart(s.p); return;
    case Step::Kind::Use:
      setDraws(s.r);
      ensureActive();
      useCalls(s.calls, s.side);
      return;
  }
}

std::shared_ptr<Running> World::launchStatus(int p, bool force) {
  return shared_->launch(p, [force](keygrant::License& license, CallOutcome& outcome) { outcome.status = license.status(force); });
}

void World::expectStatus(const Running& running) {
  if (running.outcome.error) fail("status-rejected", *running.outcome.error);
  if (!running.outcome.status) fail("status-rejected", "a call that was not a status");
}

/// Time passes, for the server and the device alike.
void World::pass(std::int64_t ms) {
  const std::int64_t now = shared_->trueNow.fetch_add(ms) + ms;
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->inner.server.now = std::max(shared_->inner.server.now, now);
}

/// A release drops the journal of the seat. The releasing process lands what it holds first, so the release carries its journal:
/// the contract lets it drop an entry only when the server got none of it (no 200) or it is past the 64 a report holds; a void
/// it was meant to carry and did not, in any other case, is a fault. What another process held only in memory is lost with the
/// seat.
void World::deactivate(int p) {
  const std::vector<SpendEntry> journal = viewEntries(p);
  const std::vector<JournalOp> others = held(1 - p);
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->inner.releases.clear();
  }
  const std::shared_ptr<Running> running =
      shared_->launch(p, [](keygrant::License& license, CallOutcome& outcome) { outcome.release = license.deactivate(); });
  shared_->settle(stepNo_);
  shared_->drive(stepNo_);
  if (running->outcome.error) {
    // It fails, the key kept, when the state cannot be saved; any other reason is a bug.
    if (!shared_->diskFailing) fail("deactivate-rejected", *running->outcome.error);
    return;
  }
  std::set<std::string> carried;
  bool applied = false;
  std::map<std::string, std::int64_t> recorded;
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    for (const auto& [served, ids] : shared_->inner.releases) {
      if (!served) continue;
      applied = true;
      carried.insert(ids.begin(), ids.end());
    }
    for (const auto& [id, heldSpend] : shared_->inner.server.recorded) recorded[id] = heldSpend.n;
  }
  for (std::size_t index = 0; index < journal.size(); ++index) {
    const SpendEntry& e = journal[index];
    // The report holds the first 64 of the journal in the order the SDK keeps it: only what the server got none of (no 200) or
    // what is past them may be dropped.
    const bool mayDrop = !applied || index >= kEntries;
    if (e.kind == SpendKind::Void) {
      bool voided = false;
      {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        voided = shared_->inner.server.voided.count(e.id) != 0;
      }
      if (carried.count(e.id) != 0 || voided) continue;
      if (!mayDrop) {
        fail("release-dropped-void", "a release left a void of the journal behind: " + lbl(e.id) + " is entry " +
                                         std::to_string(index) + " of " + std::to_string(journal.size()) + " (" +
                                         std::to_string(carried.size()) + " carried)");
      }
      std::lock_guard<std::mutex> lock(shared_->mutex);
      shared_->inner.lostVoids.insert(e.id);
    } else if (mayDrop) {
      // What the server holds of it already is not lost; the rest is, and the contract allows it.
      const auto heldHere = recorded.find(e.id);
      credit(lossAllowed_, e.meter, std::max<std::int64_t>(0, e.n - (heldHere == recorded.end() ? 0 : heldHere->second)));
    }
  }
  lose(others);
}

/// What a process held only in memory is lost with it: the spends are the customer's to have been told of.
void World::lose(const std::vector<JournalOp>& ops) {
  for (const JournalOp& op : ops) {
    if (!op.isSpend()) continue;
    if (op.isVoid()) {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      shared_->inner.lostVoids.insert(op.id);
    } else {
      credit(lossAllowed_, op.meter, op.n);
    }
  }
}

/// The app ends and starts again: what its process held only in memory is gone, and the customer was told it was made.
void World::restart(int p) {
  lose(held(p));
  heldVoids_[p].clear();
  std::shared_ptr<keygrant::License> fresh = shared_->process(p);
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->inner.procs[static_cast<std::size_t>(p)] = std::move(fresh);
}

/// The seat is taken (on a good network and disk) when a release dropped it: the app activates again.
void World::ensureActive() {
  if (current().key) return;
  Net net = Net::Good;
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    net = shared_->inner.net;
    shared_->inner.net = Net::Good;
  }
  const bool disk = shared_->diskFailing;
  shared_->diskFailing = false;
  const std::shared_ptr<Running> running =
      shared_->launch(0, [](keygrant::License& license, CallOutcome& outcome) { outcome.activate = license.activate(KEY); });
  shared_->settle(stepNo_);
  shared_->drive(stepNo_);
  if (running->outcome.error) fail("reactivation", *running->outcome.error);
  if (!running->outcome.activate || !running->outcome.activate->ok) fail("reactivation", "the activation was refused");
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->inner.net = net;
  }
  shared_->diskFailing = disk;
}

Pre World::preOf(const Call& call) {
  const std::optional<StoredState> seen = keygrant::detail::LicenseAccess::seenState(*proc(call.p));
  const StoredState state = seen ? *seen : StoredState{};
  const std::vector<SpendEntry>& spends = state.spends;
  const auto figure = baseFigure(state, call.meter);
  const std::int64_t unsynced = unsyncedIn(spends, call.meter);
  const std::int64_t clockNow = shared_->clockNow();
  const std::int64_t guarded = std::max(clockNow, state.lastSeen.value_or(0));
  const std::int64_t left = figure ? std::max<std::int64_t>(0, figure->first - unsynced) : 0;
  Pre pre;
  pre.licensed = state.key.has_value() && state.lease.has_value();
  pre.known = figure.has_value();
  pre.covered = figure && unsynced + call.n <= figure->second && call.n <= left;
  pre.leftAfter = left - call.n;
  pre.entries = spends.size();
  pre.openOffline = std::any_of(spends.begin(), spends.end(), [&](const SpendEntry& e) {
    return e.kind == SpendKind::Offline && e.meter == call.meter;
  });
  pre.waiting = askWait(state, clockNow) || journalWait(spends, guarded);
  pre.voidWaiting = voidWait(spends, guarded, clockNow);
  return pre;
}

void World::useCalls(const std::vector<Call>& calls, const std::optional<std::pair<int, bool>>& side) {
  const bool concurrent = calls.size() > 1 || side.has_value();
  // Behind a call that has not finished a process saves nothing: what it spends offline is kept in memory, and told.
  bool behind = side && std::any_of(calls.begin(), calls.end(), [&](const Call& c) { return c.p == side->first; });
  for (std::size_t i = 0; i < calls.size() && !behind; ++i) {
    for (std::size_t j = i + 1; j < calls.size(); ++j) {
      if (calls[i].p == calls[j].p) behind = true;
    }
  }
  const bool risky = shared_->diskFailing || behind;
  std::vector<Pre> pres;
  for (const Call& c : calls) pres.push_back(preOf(c));
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    for (std::vector<Req>& reqs : shared_->inner.useReqs) reqs.clear();
  }
  std::vector<std::shared_ptr<Running>> running;
  for (const Call& call : calls) {
    running.push_back(shared_->launch(call.p, [call](keygrant::License& license, CallOutcome& outcome) {
      outcome.use = license.use(call.meter, call.n);
    }));
    shared_->settle(stepNo_);
  }
  std::shared_ptr<Running> sideRun;
  if (side) {
    sideRun = launchStatus(side->first, side->second);
    shared_->settle(stepNo_);
  }
  shared_->drive(stepNo_);
  // What each process sees now: a spend after the first of two may meet a journal the first filled.
  const std::vector<SpendEntry> views[2] = {viewEntries(0), viewEntries(1)};
  for (std::size_t i = 0; i < calls.size(); ++i) {
    const Call& call = calls[i];
    if (running[i]->outcome.error) fail("use-rejected", *running[i]->outcome.error);
    if (!running[i]->outcome.use) fail("use-rejected", "a call that was not a spend");
    const UseResult result = *running[i]->outcome.use;
    std::vector<Req> reqs;
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      for (const Req& q : shared_->inner.useReqs[call.p]) {
        if (q.meter == call.meter) reqs.push_back(q);
      }
    }
    if (tracing()) {
      std::ostringstream shown;
      shown << result;
      note("  p" + std::to_string(call.p) + " use " + call.meter + " " + std::to_string(call.n) + " => " + shown.str());
    }
    judge(call, pres[i], result, reqs, concurrent, risky, views[call.p]);
  }
  if (sideRun) expectStatus(*sideRun);
}

/// What the contract says of an answer, and the ledger of what the customer was told.
void World::judge(const Call& call, const Pre& pre, const UseResult& res, const std::vector<Req>& reqs, bool concurrent, bool risky,
                  const std::vector<SpendEntry>& view) {
  const bool asked = !reqs.empty();
  const bool alone = !concurrent && pre.licensed;
  const std::string where = std::to_string(call.n) + " of " + call.meter;
  if (alone && !pre.covered && pre.voidWaiting && asked) {
    fail("uncovered-asked-in-wait", where + " asked the server while a void under a minute old stands");
  }
  if (alone && pre.covered && asked == pre.waiting) {
    fail("covered-ask-rule", where + (asked ? " asked the server while a wait runs" : " did not ask the server though no wait runs"));
  }
  if (pre.licensed && res.ok && !pre.known) {
    fail("ghost-spent", where + " was spent though the key has no such meter");
  }
  if (pre.licensed && !res.ok) {
    // This world's server gives no verdict, a read of the state never fails and its amounts are whole: a licensed call is refused
    // for the allowance, for a meter the key lacks (the only no-meter), or with used-up, the answer of a request the server
    // answered with fewer left than asked.
    const UseError error = res.error.value_or(UseError::Storage);
    const bool usedUpAfterAnAsk = error == UseError::UsedUp && asked && (!res.left || *res.left < call.n);
    if (!(error == UseError::OfflineLimit || (error == UseError::NoMeter && !pre.known) || usedUpAfterAnAsk)) {
      fail("unexpected-refusal", where + " was refused as " + keygrant::to_string(error) + ", which nothing in this world can mean");
    }
    // A refusal for the allowance always tells what is left; for a lone call it is the figure less the unsynced, as it stood
    // before the spend.
    if (error == UseError::OfflineLimit && !res.left) fail("left-missing", where + " was refused for the allowance and said nothing of what is left");
    if (alone && error == UseError::OfflineLimit && res.left != std::optional<std::int64_t>(pre.leftAfter + call.n)) {
      fail("left-wrong", where + " was refused for the allowance and answered " + (res.left ? std::to_string(*res.left) : std::string("none")) +
                             " left, the figure and the unsynced say " + std::to_string(pre.leftAfter + call.n));
    }
  }
  if (res.ok) {
    // What is left is never told high. Made offline it is the figure less the unsynced, less the spend; online the server's own
    // is at most what it has.
    const std::int64_t left = res.left.value_or(0);
    if (alone && res.offline && left != pre.leftAfter) {
      fail("left-wrong", where + " made offline answered " + std::to_string(left) + " left, the figure and the unsynced say " +
                             std::to_string(pre.leftAfter));
    }
    std::int64_t serverLeft = 0;
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      serverLeft = shared_->inner.server.left(call.meter);
    }
    if (alone && !res.offline && left > serverLeft) {
      fail("left-high", where + " answered " + std::to_string(left) + " left, the server has " + std::to_string(serverLeft));
    }
    credit(acked_, call.meter, call.n);
    if (res.offline && risky) {
      credit(told_, call.meter, call.n);
      ++toldOps_;
    }
    return;
  }
  // A void finds no room when the journal, less the oldest void if sixteen stand and the new one takes its place, still holds the
  // cap; one that landed, or is held, found its room.
  const std::size_t viewVoids = static_cast<std::size_t>(
      std::count_if(view.begin(), view.end(), [](const SpendEntry& e) { return e.kind == SpendKind::Void; }));
  const bool crowded = std::max(pre.entries, view.size()) - (viewVoids >= kVoids ? 1 : 0) >= kEntries;
  std::vector<JournalOp> heldOps;
  for (int p = 0; p < 2; ++p) {
    for (const JournalOp& op : held(p)) heldOps.push_back(op);
  }
  const auto placed = [&](const std::string& id) {
    return std::any_of(view.begin(), view.end(), [&](const SpendEntry& e) { return e.id == id; }) ||
           std::any_of(heldOps.begin(), heldOps.end(), [&](const JournalOp& op) { return op.isVoid() && op.id == id; });
  };
  // A process's requests of one meter are not told apart by call, so a spend the customer was told was made (another call of the
  // step, asked and unanswered, then kept as a plain entry or reported as one) is not a refused spend whose void is owed.
  std::set<std::string> reportedPlain;
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    reportedPlain = shared_->inner.reportedPlain;
  }
  const auto toldId = [&](const std::string& id) {
    return reportedPlain.count(id) != 0 ||
           std::any_of(view.begin(), view.end(), [&](const SpendEntry& e) { return e.id == id && e.kind != SpendKind::Void; }) ||
           std::any_of(heldOps.begin(), heldOps.end(), [&](const JournalOp& op) { return op.isSpend() && !op.isVoid() && op.id == id; });
  };
  for (const Req& q : reqs) {
    if (q.applied && !q.answered && !toldId(q.id)) phantoms_.push_back(Phantom{q.id, q.meter, q.n, crowded && !placed(q.id)});
  }
  if (concurrent || !pre.licensed || (res.error != UseError::OfflineLimit && res.error != UseError::NoMeter)) return;
  if (pre.covered && res.error == UseError::OfflineLimit) {
    // A covered spend is refused only for the journal's size: it would open a 65th entry.
    const bool capped = pre.entries >= kEntries && (asked || !pre.openOffline);
    if (!capped) {
      fail("covered-refused", where + " was covered by the allowance and refused (journal of " + std::to_string(pre.entries) + ")");
    }
  }
  if (!pre.covered && !asked && !pre.voidWaiting) {
    fail("uncovered-unasked", where + " was refused without asking, and no void wait runs");
  }
}

void World::describe() {
  const auto show = [this](const SpendEntry& e) {
    return std::string(keygrant::to_string(e.kind)) + ":" + lbl(e.id) + ":" + e.meter + ":" + std::to_string(e.n);
  };
  const auto showOp = [this](const JournalOp& op) {
    using Kind = JournalOp::Kind;
    switch (op.kind) {
      case Kind::Confirmed: return "confirmed:" + std::to_string(op.entries.size());
      case Kind::Figures: return std::string("figures");
      case Kind::Activated: return std::string("activated");
      case Kind::Deactivated: return std::string("deactivated");
      case Kind::Spend: break;
    }
    return std::string(keygrant::to_string(op.spend)) + (op.told ? "!" : "") + ":" + lbl(op.id) + ":" + op.meter + ":" + std::to_string(op.n);
  };
  std::string stored;
  for (const SpendEntry& e : current().spends) stored += (stored.empty() ? "" : " ") + show(e);
  note("  stored [" + stored + "]");
  for (int p = 0; p < 2; ++p) {
    std::string heldHere;
    for (const JournalOp& op : held(p)) heldHere += (heldHere.empty() ? "" : " ") + showOp(op);
    note("  p" + std::to_string(p) + " holds [" + heldHere + "]");
  }
}

// --- what must hold between steps, and at the end -------------------------------------------------------------------------------

/// A void held in memory that is gone, though it neither landed nor reached the server: sixteen were held and the oldest went, or
/// the journal found no room for it.
void World::accountVoids() {
  const std::vector<SpendEntry> stored = current().spends;
  for (int p = 0; p < 2; ++p) {
    std::set<std::string> now;
    for (const JournalOp& op : held(p)) {
      if (op.isVoid()) now.insert(op.id);
    }
    const std::set<std::string> before = heldVoids_[p];
    for (const std::string& id : before) {
      bool voided = false;
      bool lost = false;
      bool refused = false;
      {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        voided = shared_->inner.server.voided.count(id) != 0;
        lost = shared_->inner.lostVoids.count(id) != 0;
        refused = shared_->inner.refusedVoids.count(id) != 0;
      }
      if (now.count(id) != 0 || std::any_of(stored.begin(), stored.end(), [&](const SpendEntry& e) { return e.id == id; }) || voided || lost) {
        continue;
      }
      if (before.size() >= kVoids || refused) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->inner.lostVoids.insert(id);
      } else {
        fail("void-dropped", "a void held in memory is gone: " + lbl(id) + " (" + std::to_string(before.size()) + " held, " +
                                 std::to_string(stored.size()) + " stored)");
      }
    }
    heldVoids_[p] = std::move(now);
  }
}

/// The phantoms the server recorded and no void has taken back.
std::vector<Phantom> World::outstanding() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  std::vector<Phantom> out;
  for (const Phantom& x : phantoms_) {
    if (shared_->inner.server.recorded.count(x.id) != 0 && shared_->inner.server.voided.count(x.id) == 0) out.push_back(x);
  }
  return out;
}

void World::check() {
  {
    std::optional<std::string> violation;
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      violation = shared_->inner.violation;
    }
    if (violation) fail("void-replaced", *violation);
  }
  const std::vector<SpendEntry> stored = current().spends;
  std::set<std::string> distinct;
  for (const SpendEntry& e : stored) distinct.insert(e.id);
  if (distinct.size() != stored.size()) fail("duplicate-id", "the journal holds one id twice");
  if (stored.size() > kEntries + toldOps_) fail("journal-size", std::to_string(stored.size()) + " entries stored, past the cap and what was told");
  if (std::count_if(stored.begin(), stored.end(), [](const SpendEntry& e) { return e.kind == SpendKind::Void; }) > static_cast<std::ptrdiff_t>(kVoids)) {
    fail("void-cap", "more voids stored than the cap");
  }
  for (const auto& [code, spec] : case_.meters) {
    const std::int64_t unsynced = unsyncedIn(stored, code);
    const std::int64_t told = told_[code];
    // What the journal began with is the device's to report, past the allowance or not; no spend lands on top of a meter that is
    // already past it, and none ever takes it further but those the customer was told were made.
    std::int64_t preloaded = 0;
    for (std::size_t i = 0; i < case_.preload.size(); ++i) {
      const Preloaded& e = case_.preload[i];
      if (e.kind == SpendKind::Void || e.meter != code) continue;
      if (distinct.count(preId(i)) != 0) preloaded += e.n;
    }
    const std::int64_t allowance = std::max(spec.offline, preloaded);
    if (unsynced > allowance + told) {
      fail("allowance", std::to_string(unsynced) + " of " + code + " unsynced past its allowance " + std::to_string(allowance) + " and the " +
                            std::to_string(told) + " told");
    }
  }
  bool holding = false;
  for (int p = 0; p < 2; ++p) {
    const std::vector<JournalOp> heldHere = held(p);
    const std::ptrdiff_t voids = std::count_if(heldHere.begin(), heldHere.end(), [](const JournalOp& op) { return op.isVoid(); });
    if (voids > static_cast<std::ptrdiff_t>(kVoids)) fail("unsaved-voids", "a process holds more voids in memory than the cap");
    if (std::any_of(heldHere.begin(), heldHere.end(), [](const JournalOp& op) { return op.isVoid() && op.told; })) {
      fail("void-told", "a void was marked told");
    }
    if (heldHere.size() > kEntries + kVoids + 16) fail("unsaved-size", std::to_string(heldHere.size()) + " changes held in memory");
    holding = holding || !heldHere.empty();
  }
  accountVoids();
  // The server never holds more than the customer was told and the refusals it recorded: a spend counted twice shows here at once.
  const std::vector<Phantom> owing = outstanding();
  for (const auto& [code, spec] : case_.meters) {
    (void)spec;
    std::int64_t phantom = 0;
    for (const Phantom& x : owing) phantom += x.meter == code ? x.n : 0;
    std::int64_t used = 0;
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      used = shared_->inner.server.used(code);
    }
    if (used > acked_[code] + phantom) {
      fail("counted-twice", code + ": the server holds " + std::to_string(used) + ", the customer was told " + std::to_string(acked_[code]) +
                                " (and " + std::to_string(phantom) + " refused but recorded)");
    }
  }
  // What was told while only memory held it excuses the allowance only while it is unreported: once a meter's journal drains,
  // the loosening ends.
  std::vector<JournalOp> heldAll;
  for (int p = 0; p < 2; ++p) {
    for (const JournalOp& op : held(p)) heldAll.push_back(op);
  }
  for (const auto& meter : case_.meters) {
    // A name, not a structured binding: a lambda capturing a binding is C++20, and Apple Clang says so.
    const auto& code = meter.first;
    const bool heldHere = std::any_of(heldAll.begin(), heldAll.end(), [&](const JournalOp& op) {
      return op.isSpend() && !op.isVoid() && op.meter == code;
    });
    if (unsyncedIn(stored, code) == 0 && !heldHere) told_[code] = 0;
  }
  if (stored.empty() && !holding) toldOps_ = 0;
}

/// The network and the disk come back, time passes, and every journal is reported: then the books are balanced.
void World::converge() {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->inner.net = Net::Good;
    shared_->inner.rs = {0.0};
    shared_->inner.rk = 0;
  }
  shared_->diskFailing = false;
  const std::int64_t now = shared_->trueNow.fetch_add(3 * kHour) + 3 * kHour;
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->inner.server.now = std::max(shared_->inner.server.now, now);
  }
  ensureActive();
  // A journal past 64 entries is reported 64 at a time, so more than one check-in each may be needed.
  for (int round = 0; round < 4 && (round == 0 || !current().spends.empty() || !held(0).empty() || !held(1).empty()); ++round) {
    for (int p = 0; p < 2; ++p) {
      const std::shared_ptr<Running> running = launchStatus(p, true);
      shared_->settle(stepNo_);
      shared_->drive(stepNo_);
      expectStatus(*running);
      accountVoids();
    }
  }
  {
    std::optional<std::string> violation;
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      violation = shared_->inner.violation;
    }
    if (violation) fail("void-replaced", *violation);
  }
  const std::vector<SpendEntry> leftOver = current().spends;
  if (!leftOver.empty()) {
    std::string shown;
    for (const SpendEntry& e : leftOver) shown += (shown.empty() ? "" : " ") + std::string(keygrant::to_string(e.kind)) + ":" + lbl(e.id);
    fail("journal-left", "the journal still holds " + shown + " after every process reported");
  }
  if (!held(0).empty() || !held(1).empty()) fail("unsaved-left", "a process still holds changes in memory");
  // A refusal the server recorded is taken back, unless that very void was lost as the contract allows: replaced by a newer one,
  // dropped with a release, held by a process that ended, or never given room.
  const std::vector<Phantom> owing = outstanding();
  for (const Phantom& x : owing) {
    bool lost = false;
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      lost = shared_->inner.lostVoids.count(x.id) != 0;
    }
    if (!x.full && !lost) {
      fail("void-lost", x.meter + ": " + std::to_string(x.n) + " refused after the server recorded it (" + lbl(x.id) +
                            "), and its void did not reach the server though nothing lost it");
    }
  }
  std::vector<LicenseStatus> shown;
  for (int p = 0; p < 2; ++p) {
    const std::shared_ptr<Running> running = shared_->launch(p, [](keygrant::License& license, CallOutcome& outcome) {
      outcome.status = license.status(false);
    });
    shared_->settle(stepNo_);
    shared_->drive(stepNo_);
    expectStatus(*running);
    shown.push_back(*running->outcome.status);
  }
  for (const auto& [code, spec] : case_.meters) {
    std::int64_t used = 0;
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      used = shared_->inner.server.used(code);
    }
    std::int64_t owed = 0;
    for (const Phantom& x : owing) owed += x.meter == code ? x.n : 0;
    const std::int64_t acked = acked_[code];
    const std::int64_t lost = lossAllowed_[code];
    if (used + lost < acked + owed) {
      fail("lost", code + ": the server holds " + std::to_string(used) + ", the customer was told " + std::to_string(acked) + " (and " +
                       std::to_string(owed) + " refused but recorded)");
    }
    if (used > acked + owed) {
      fail("counted-twice", code + ": the server holds " + std::to_string(used) + ", the customer was told " + std::to_string(acked) +
                                " (and " + std::to_string(owed) + " refused but recorded)");
    }
    const std::int64_t left = std::max<std::int64_t>(0, spec.total - used);
    for (const LicenseStatus& status : shown) {
      const auto figure = status.meters.find(code);
      if (figure == status.meters.end() || figure->second.left != left || figure->second.unsynced != 0) {
        std::ostringstream text;
        if (figure != status.meters.end()) {
          text << "left " << figure->second.left << ", unsynced " << figure->second.unsynced;
        } else {
          text << "none";
        }
        fail("figures", code + ": status shows " + text.str() + ", the server has " + std::to_string(left) + " left");
      }
    }
  }
}

std::pair<std::optional<PropertyFailure>, std::vector<std::string>> runCase(const Case& c, bool trace) {
  World world(c, trace);
  std::optional<PropertyFailure> failure;
  try {
    world.run();
  } catch (const PropertyFailure& f) {
    failure = f;
  } catch (const std::exception& e) {
    failure = PropertyFailure("exception", e.what());
  }
  return {failure, world.trace()};
}

}  // namespace kgtest::property
