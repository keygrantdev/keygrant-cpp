#include "property/property_case.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kgtest::property {

namespace {

json callJson(const Call& call) { return {{"p", call.p}, {"meter", call.meter}, {"n", call.n}}; }

Call callOf(const json& value) {
  return Call{value.at("p").get<int>(), value.at("meter").get<std::string>(), value.at("n").get<std::int64_t>()};
}

}  // namespace

const char* Step::name() const {
  switch (kind) {
    case Kind::Use: return "use";
    case Kind::Status: return "status";
    case Kind::Repeat: return "repeat";
    case Kind::Deactivate: return "deactivate";
    case Kind::Restart: return "restart";
    case Kind::Wait: return "wait";
    case Kind::Jump: return "jump";
    case Kind::Net: return "net";
    case Kind::Disk: return "disk";
  }
  return "wait";
}

json Step::toJson() const {
  json out = {{"t", name()}};
  switch (kind) {
    case Kind::Use: {
      json list = json::array();
      for (const Call& c : calls) list.push_back(callJson(c));
      out["calls"] = list;
      if (side) out["status"] = {{"p", side->first}, {"force", side->second}};
      break;
    }
    case Kind::Status:
      out["p"] = p;
      out["force"] = force;
      break;
    case Kind::Repeat:
      out["call"] = callJson(call);
      out["times"] = times;
      out["gapMs"] = gapMs;
      break;
    case Kind::Deactivate:
    case Kind::Restart: out["p"] = p; break;
    case Kind::Wait:
    case Kind::Jump: out["ms"] = ms; break;
    case Kind::Net: out["mode"] = mode; break;
    case Kind::Disk: out["failing"] = failing; break;
  }
  if (!r.empty()) out["r"] = r;
  return out;
}

Step Step::fromJson(const json& value) {
  Step step;
  const std::string t = value.at("t").get<std::string>();
  if (value.contains("r")) step.r = value["r"].get<std::vector<double>>();
  const auto pOf = [&value] { return value.value("p", 0); };
  if (t == "use") {
    step.kind = Kind::Use;
    for (const json& c : value.at("calls")) step.calls.push_back(callOf(c));
    if (value.contains("status") && value["status"].is_object()) {
      step.side = std::make_pair(value["status"].at("p").get<int>(), value["status"].at("force").get<bool>());
    }
  } else if (t == "status") {
    step.kind = Kind::Status;
    step.p = pOf();
    step.force = value.at("force").get<bool>();
  } else if (t == "repeat") {
    step.kind = Kind::Repeat;
    step.call = callOf(value.at("call"));
    step.times = value.at("times").get<std::int64_t>();
    step.gapMs = value.at("gapMs").get<std::int64_t>();
  } else if (t == "deactivate") {
    step.kind = Kind::Deactivate;
    step.p = pOf();
  } else if (t == "restart") {
    step.kind = Kind::Restart;
    step.p = pOf();
  } else if (t == "wait") {
    step.kind = Kind::Wait;
    step.ms = value.at("ms").get<std::int64_t>();
  } else if (t == "jump") {
    step.kind = Kind::Jump;
    step.ms = value.at("ms").get<std::int64_t>();
  } else if (t == "net") {
    step.kind = Kind::Net;
    step.mode = value.at("mode").get<std::string>();
  } else if (t == "disk") {
    step.kind = Kind::Disk;
    step.failing = value.at("failing").get<bool>();
  } else {
    throw std::invalid_argument("a step this test does not know: " + t);
  }
  return step;
}

json Case::toJson() const {
  json out = {{"seed", seed}};
  json meterList = json::object();
  for (const auto& [code, m] : meters) meterList[code] = {{"total", m.total}, {"offline", m.offline}, {"resets", m.resets}};
  out["meters"] = meterList;
  json entries = json::array();
  for (const Preloaded& e : preload) {
    entries.push_back({{"meter", e.meter},
                       {"n", e.n},
                       {"kind", keygrant::to_string(e.kind)},
                       {"agoMs", e.agoMs},
                       {"recorded", e.recorded}});
  }
  out["preload"] = entries;
  json stepList = json::array();
  for (const Step& s : steps) stepList.push_back(s.toJson());
  out["steps"] = stepList;
  return out;
}

Case Case::fromJson(const json& value) {
  Case c;
  c.seed = value.at("seed").get<std::int64_t>();
  for (const auto& [code, m] : value.at("meters").items()) {
    c.meters[code] = MeterSpec{m.at("total").get<std::int64_t>(), m.at("offline").get<std::int64_t>(), m.at("resets").get<bool>()};
  }
  for (const json& e : value.at("preload")) {
    c.preload.push_back(Preloaded{e.at("meter").get<std::string>(), e.at("n").get<std::int64_t>(),
                                  *keygrant::spendKindFromString(e.at("kind").get<std::string>()),
                                  e.at("agoMs").get<std::int64_t>(), e.at("recorded").get<bool>()});
  }
  for (const json& s : value.at("steps")) c.steps.push_back(Step::fromJson(s));
  return c;
}

Case Case::withSteps(std::vector<Step> replaced) const {
  Case other = *this;
  other.steps = std::move(replaced);
  return other;
}

double Rng::next() {
  state_ += 0x6d2b79f5u;
  const std::uint32_t a = state_;
  std::uint32_t t = (a ^ (a >> 15)) * (1u | a);
  t = (t + (t ^ (t >> 7)) * (61u | t)) ^ t;
  return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
}

namespace {

/// The draws of the reference's generator, in the order it makes them.
class Gen {
 public:
  explicit Gen(std::uint32_t seed) : rng_(seed) {}

  double rand() { return rng_.next(); }

  std::int64_t integer(std::int64_t lo, std::int64_t hi) {
    return lo + static_cast<std::int64_t>(std::floor(rand() * static_cast<double>(hi - lo + 1)));
  }

  template <class T>
  T pick(const std::vector<T>& xs) {
    return xs[static_cast<std::size_t>(std::floor(rand() * static_cast<double>(xs.size())))];
  }

  std::string code() {
    if (rand() < 0.1) return "ghost";
    return pick<std::string>({"exports", "exports", "renders", "credits"});
  }

  std::int64_t amount() {
    if (rand() < 0.7) return 1;
    if (rand() < 0.8) return integer(2, 5);
    if (rand() < 0.7) return integer(6, 14);
    return 500;
  }

  std::vector<double> draws() {
    std::vector<double> out;
    for (int i = 0; i < 6; ++i) out.push_back(rand());
    return out;
  }

  /// A call: the process (drawn first when not given), then the meter (drawn again while it is `not`), then the amount.
  Call call(std::optional<int> p = std::nullopt, const std::optional<std::string>& not_ = std::nullopt) {
    const int process = p ? *p : static_cast<int>(integer(0, 1));
    std::string meter = code();
    while (not_ && meter == *not_) meter = code();
    const std::int64_t n = amount();
    return Call{process, meter, n};
  }

 private:
  Rng rng_;
};

Step useStep(std::vector<Call> calls, std::optional<std::pair<int, bool>> side, std::vector<double> r) {
  Step step;
  step.kind = Step::Kind::Use;
  step.calls = std::move(calls);
  step.side = side;
  step.r = std::move(r);
  return step;
}

Step netStep(const std::string& mode) {
  Step step;
  step.kind = Step::Kind::Net;
  step.mode = mode;
  return step;
}

Step diskStep(bool failing) {
  Step step;
  step.kind = Step::Kind::Disk;
  step.failing = failing;
  return step;
}

Step waitStep(Step::Kind kind, std::int64_t ms) {
  Step step;
  step.kind = kind;
  step.ms = ms;
  return step;
}

Step deactivateStep(int p, std::vector<double> r) {
  Step step;
  step.kind = Step::Kind::Deactivate;
  step.p = p;
  step.r = std::move(r);
  return step;
}

}  // namespace

Case generate(std::int64_t seed) {
  Gen g(static_cast<std::uint32_t>(seed));
  Case c;
  c.seed = seed;
  MeterSpec exports{0, 0, false};
  exports.total = g.integer(8, 150);
  exports.offline = g.integer(1, 12);
  MeterSpec renders{0, 0, true};
  renders.total = g.integer(3, 40);
  MeterSpec credits{0, 0, false};
  credits.total = g.integer(4, 60);
  credits.offline = g.integer(0, 6);
  c.meters = {{"exports", exports}, {"renders", renders}, {"credits", credits}};
  const std::vector<std::string> all = {"exports", "renders", "credits"};
  // A storm: sixteen voids or one short of it, an outage, and spends the allowance cannot cover a minute apart: the cap is reached.
  const bool storm = g.rand() < 0.12;
  // A journal past the cap, which a release reports 64 of.
  const bool overfull = !storm && g.rand() < 0.06;
  if (storm || overfull || g.rand() < 0.4) {
    std::vector<std::string> offline;
    for (const std::string& m : all) {
      if (g.rand() < 0.3) offline.push_back(m);
    }
    // How many of the entries are voids is drawn once: sixteen or one short of it in a storm, else none to sixteen.
    const std::int64_t voids = storm ? g.integer(14, 16) : overfull ? g.integer(0, 13) : g.integer(0, 16);
    std::int64_t count = 0;
    if (overfull) {
      count = g.integer(65, 70);
    } else {
      const std::int64_t wanted = storm ? voids + g.integer(0, 6) : g.pick<std::int64_t>({1, 2, 3, 5, 8, 20, 40, 55, 62, 64});
      count = std::min<std::int64_t>(wanted, 64 - static_cast<std::int64_t>(offline.size()));
    }
    for (std::int64_t i = 0; i < count; ++i) {
      Preloaded entry;
      entry.kind = i < voids ? keygrant::SpendKind::Void : keygrant::SpendKind::Unanswered;
      entry.meter = g.pick(all);
      entry.n = g.integer(1, 2);
      entry.agoMs = storm ? g.pick<std::int64_t>({5 * kMinute, 2 * kHour, 20 * kHour})
                          : g.pick<std::int64_t>({1'000, 30'000, 5 * kMinute, 2 * kHour, 20 * kHour});
      entry.recorded = g.rand() < 0.5;
      c.preload.push_back(entry);
    }
    for (const std::string& m : offline) {
      Preloaded entry;
      entry.meter = m;
      entry.n = g.integer(1, 3);
      entry.kind = keygrant::SpendKind::Offline;
      entry.agoMs = g.pick<std::int64_t>({5 * kMinute, 2 * kHour});
      entry.recorded = g.rand() < 0.5;
      c.preload.push_back(entry);
    }
    // Stable, as Array.prototype.sort is: the oldest first.
    std::stable_sort(c.preload.begin(), c.preload.end(), [](const Preloaded& a, const Preloaded& b) { return a.agoMs > b.agoMs; });
    // Past the cap, the newest entries are voids: the release carries 64 and leaves these behind.
    if (overfull) {
      for (std::int64_t k = g.integer(1, 3); k > 0; --k) {
        Preloaded entry;
        entry.meter = g.pick(all);
        entry.n = 1;
        entry.kind = keygrant::SpendKind::Void;
        entry.agoMs = 500;
        entry.recorded = g.rand() < 0.7;
        c.preload.push_back(entry);
      }
    }
  }

  std::vector<Step>& steps = c.steps;
  if (storm) {
    steps.push_back(netStep("down"));
    const int p = static_cast<int>(g.integer(0, 1));
    Step repeat;
    repeat.kind = Step::Kind::Repeat;
    repeat.call = Call{p, "renders", 1};
    repeat.times = g.integer(3, 8);
    repeat.gapMs = 61'000;
    repeat.r = g.draws();
    steps.push_back(repeat);
  } else if (overfull) {
    steps.push_back(netStep("good"));
    const int p = static_cast<int>(g.integer(0, 1));
    steps.push_back(deactivateStep(p, g.draws()));
  }
  for (std::int64_t left = g.integer(8, 26); left > 0; --left) {
    const double x = g.rand();
    if (x < 0.3) {
      Call call = g.call();
      steps.push_back(useStep({call}, std::nullopt, g.draws()));
    } else if (x < 0.34) {
      Step repeat;
      repeat.kind = Step::Kind::Repeat;
      repeat.call = g.call();
      repeat.times = g.integer(2, 14);
      repeat.gapMs = g.pick<std::int64_t>({10'000, 61'000, 5 * kMinute, 70 * kMinute});
      repeat.r = g.draws();
      steps.push_back(repeat);
    } else if (x < 0.42) {
      Call a = g.call(0);
      Call b = g.call(1);
      steps.push_back(useStep({a, b}, std::nullopt, g.draws()));
    } else if (x < 0.48) {
      Call first = g.call();
      Call second = g.call(first.p, first.meter);
      steps.push_back(useStep({first, second}, std::nullopt, g.draws()));
    } else if (x < 0.53) {
      Call call = g.call();
      const int p = static_cast<int>(g.integer(0, 1));
      const bool force = g.rand() < 0.7;
      steps.push_back(useStep({call}, std::make_pair(p, force), g.draws()));
    } else if (x < 0.64) {
      Step status;
      status.kind = Step::Kind::Status;
      status.p = static_cast<int>(g.integer(0, 1));
      status.force = g.rand() < 0.5;
      status.r = g.draws();
      steps.push_back(status);
    } else if (x < 0.78) {
      steps.push_back(waitStep(Step::Kind::Wait, g.pick<std::int64_t>({1'000, 20'000, 59'000, 61'000, 5 * kMinute, 70 * kMinute,
                                                                       3 * kHour, 26 * kHour})));
    } else if (x < 0.84) {
      steps.push_back(waitStep(Step::Kind::Jump, g.pick<std::int64_t>({30'000, 10 * kMinute, 2 * kHour, 30 * kHour, -30'000,
                                                                       -10 * kMinute, -2 * kHour, -30 * kHour})));
    } else if (x < 0.92) {
      steps.push_back(netStep(g.pick<std::string>({"good", "down", "down", "flaky", "flaky"})));
    } else if (x < 0.97) {
      steps.push_back(diskStep(g.rand() < 0.6));
    } else if (x < 0.985) {
      Step restart;
      restart.kind = Step::Kind::Restart;
      restart.p = static_cast<int>(g.integer(0, 1));
      steps.push_back(restart);
    } else {
      const int p = static_cast<int>(g.integer(0, 1));
      steps.push_back(deactivateStep(p, g.draws()));
    }
  }
  // A release after an outage: spends made while the disk refused, the disk back, and a deactivate, which must carry them.
  if (g.rand() < 0.1) {
    const int p = static_cast<int>(g.integer(0, 1));
    steps.push_back(netStep("down"));
    steps.push_back(diskStep(true));
    const std::string meter = g.pick<std::string>({"exports", "credits"});
    steps.push_back(useStep({Call{p, meter, 1}}, std::nullopt, g.draws()));
    steps.push_back(useStep({Call{p, "exports", 1}}, std::nullopt, g.draws()));
    steps.push_back(diskStep(false));
    steps.push_back(netStep("good"));
    steps.push_back(deactivateStep(p, g.draws()));
  }
  return c;
}

}  // namespace kgtest::property
