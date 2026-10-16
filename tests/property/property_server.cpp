#include "property/property_server.hpp"

#include <algorithm>

namespace kgtest::property {

std::int64_t PropServer::used(const std::string& code) const {
  std::int64_t sum = 0;
  for (const auto& [id, held] : recorded) {
    (void)id;
    if (held.meter == code) sum += held.n;
  }
  return sum;
}

std::int64_t PropServer::left(const std::string& code) const {
  const auto spec = meters.find(code);
  if (spec == meters.end()) return 0;
  return std::max<std::int64_t>(0, spec->second.total - used(code));
}

json PropServer::figures() const {
  json out = json::object();
  for (const auto& [code, m] : meters) {
    json figure = {{"left", left(code)}, {"total", m.total}, {"offline", m.offline}};
    if (m.resets) figure["resets"] = true;
    out[code] = figure;
  }
  return out;
}

std::string PropServer::lease(const std::string& aid) const {
  LeaseSpec spec;
  spec.aid = aid;
  spec.model = "subscription";
  spec.lim = 3;
  spec.major = 3;
  spec.ttlSeconds = 60 * 86'400;
  spec.now = now;
  spec.extra["met"] = ordered_json::parse(figures().dump());
  return key_.sign(claimsOf(spec));
}

/// Take a request's reports, and name the entries held after (always accepted).
json PropServer::report(const json& spends) {
  json confirmed = json::array();
  if (!spends.is_array()) return confirmed;
  for (const json& e : spends) {
    const std::string id = e.value("id", std::string());
    const bool isVoid = e.contains("void");
    const std::int64_t n = e.value("n", std::int64_t{0});
    if (voided.count(id) != 0) {
      confirmed.push_back(isVoid ? json{{"id", id}, {"void", true}} : json{{"id", id}, {"n", n}});
      continue;
    }
    if (isVoid) {
      recorded.erase(id);
      voided.insert(id);
      confirmed.push_back({{"id", id}, {"void", true}});
      continue;
    }
    const auto held = recorded.find(id);
    if (held == recorded.end()) {
      recorded[id] = Held{e.value("meter", std::string()), n};
    } else if (n > held->second.n) {
      held->second.n = n;
    }
    confirmed.push_back({{"id", id}, {"n", n}});
  }
  return confirmed;
}

Served PropServer::handle(const std::string& action, const json& body) {
  ++now;
  const json confirmed = report(body.contains("spends") ? body["spends"] : json(nullptr));
  const auto named = [&confirmed](json answer) {
    if (!confirmed.empty()) answer["spends"] = confirmed;
    return answer;
  };
  if (action == "use") return useMeter(body, named(json::object()));
  if (action == "validate") {
    return Served{200, named({{"lease", lease(body.value("activationId", std::string()))}}), std::nullopt};
  }
  if (action == "deactivate") return Served{200, named({{"released", true}}), std::nullopt};
  if (action == "activate") {
    ++activations_;
    const std::string aid = "act_" + std::to_string(activations_ + 1);
    return Served{200, {{"lease", lease(aid)}, {"activationId", aid}}, std::nullopt};
  }
  return Served{404, {{"error", "unknown"}}, std::nullopt};
}

Served PropServer::useMeter(const json& body, const json& named) {
  const std::string meter = body.value("meter", std::string());
  const std::int64_t n = body.value("n", std::int64_t{0});
  const std::string id = body.value("id", std::string());
  const auto withNamed = [&named](json answer) {
    for (const auto& [name, value] : named.items()) answer[name] = value;
    return answer;
  };
  if (meters.count(meter) == 0) return Served{404, withNamed({{"error", "unknown-meter"}}), std::nullopt};
  if (recorded.count(id) != 0) return Served{200, withNamed({{"spent", true}, {"meters", figures()}, {"at", now}}), std::nullopt};
  if (n > left(meter)) {
    return Served{409, withNamed({{"error", "used-up"}, {"left", left(meter)}, {"meters", figures()}, {"at", now}}), std::nullopt};
  }
  recorded[id] = Held{meter, n};
  return Served{200, withNamed({{"spent", true}, {"meters", figures()}, {"at", now}}), id};
}

}  // namespace kgtest::property
