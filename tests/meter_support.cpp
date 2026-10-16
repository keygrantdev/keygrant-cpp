#include "meter_support.hpp"

#include <sstream>

#include "crypto.hpp"

namespace kgtest {

std::optional<std::filesystem::path> aboveTests(const std::string& relative) {
  std::filesystem::path folder = std::filesystem::path(__FILE__).parent_path();
  for (int i = 0; i < 10 && !folder.empty(); ++i) {
    for (const std::filesystem::path& candidate : {folder / relative, folder / "sdk" / relative}) {
      std::error_code ignored;
      if (std::filesystem::is_regular_file(candidate, ignored)) return candidate;
    }
    if (folder == folder.parent_path()) break;
    folder = folder.parent_path();
  }
  return std::nullopt;
}

bool inVectorsRepository() {
  static const bool in = aboveTests("scripts/export-sdk.mjs").has_value();
  return in;
}

std::string spendId(const std::string& who) {
  std::string id = who.size() == 1 ? std::string(22, who[0]) : who;
  while (id.size() < 22) id.push_back('x');
  return id.substr(0, 22);
}

SpendEntry entry(SpendKind kind, const std::string& who, std::int64_t n, std::int64_t at, const std::string& meter) {
  return SpendEntry{spendId(who), meter, n, at, kind};
}

json spendJson(const SpendEntry& e) {
  return {{"id", e.id}, {"meter", e.meter}, {"n", e.n}, {"at", e.at}, {"kind", keygrant::to_string(e.kind)}};
}

json spendsJson(const std::vector<SpendEntry>& spends) {
  json out = json::array();
  for (const SpendEntry& e : spends) out.push_back(spendJson(e));
  return out;
}

json figureJson(const MeterFigure& f) {
  return {{"left", f.left}, {"total", f.total}, {"offline", f.offline}, {"resets", f.resets}, {"at", f.at}};
}

json figuresJson(const std::map<std::string, MeterFigure>& figures) {
  json out = json::object();
  for (const auto& [code, f] : figures) out[code] = figureJson(f);
  return out;
}

json meterStatusJson(const std::map<std::string, MeterStatus>& meters) {
  json out = json::object();
  for (const auto& [code, m] : meters) {
    out[code] = {{"left", m.left}, {"total", m.total}, {"offline", m.offline}, {"unsynced", m.unsynced}, {"resets", m.resets}};
  }
  return out;
}

MeterSpecs defaultMeters() {
  return {{"exports", MeterSpec{90, 100, 10, false}}, {"renders", MeterSpec{5, 5, 0, true}}};
}

json metClaim(const MeterSpecs& meters) {
  json out = json::object();
  for (const auto& [code, m] : meters) {
    out[code] = {{"left", m.left}, {"total", m.total}, {"offline", m.offline}};
    if (m.resets) out[code]["resets"] = true;
  }
  return out;
}

std::string meterLease(const Harness& h, const MeterSpecs& meters, std::int64_t now) {
  LeaseSpec spec;
  spec.model = "subscription";
  spec.major = 3;
  spec.now = now;
  spec.extra["met"] = metClaim(meters);
  return h.sign(spec);
}

std::string rawMetLease(const Harness& h, const std::string& metText, std::int64_t now) {
  ordered_json claims = claimsOf(LeaseSpec{});
  claims["model"] = "perpetual";
  claims["major"] = 3;
  const std::int64_t iat = now / 1000;
  claims["iat"] = iat;
  claims["exp"] = iat + 30 * 24 * 3600;
  std::string payload = claims.dump();
  // The claim written into the payload as given: `met` is a member of its own, last.
  payload.pop_back();
  payload += ",\"met\":" + metText + "}";
  return h.key.signRaw(payload);
}

StoredState meterState(const Harness& h, const MeterSpecs& meters, const std::vector<SpendEntry>& spends,
                       std::int64_t now) {
  StoredState state = held(meterLease(h, meters, now), KEY, 1);
  state.spends = spends;
  return state;
}

json spentBody(std::int64_t left, std::int64_t at) {
  return {{"spent", true}, {"meters", {{"exports", {{"left", left}, {"total", 100}, {"offline", 10}}}}}, {"at", at}};
}

MeterHarness::MeterHarness(const MeterSpecs& meters, const std::vector<SpendEntry>& spends, HarnessOptions options)
    : Harness(std::nullopt, std::move(options)) {
  store(meterState(*this, meters, spends));
}

std::vector<SpendEntry> MeterHarness::journal() {
  const std::optional<StoredState> state = stored();
  return state ? state->spends : std::vector<SpendEntry>{};
}

std::string MeterHarness::shape() {
  std::ostringstream out;
  const char* separator = "";
  for (const SpendEntry& e : journal()) {
    out << separator << keygrant::to_string(e.kind) << ":" << e.n;
    separator = " ";
  }
  return out.str();
}

json MeterHarness::useBody(std::size_t nth) {
  const std::vector<json> bodies = http->bodiesOf("use");
  if (nth >= bodies.size()) {
    ADD_FAILURE() << "expected use request " << nth << ", there were " << bodies.size();
    return json::object();
  }
  return bodies[nth];
}

std::map<std::string, MeterStatus> MeterHarness::meters() { return license().status().meters; }

void MeterHarness::grantEdition(const std::string& edition, const MeterSpecs& meters) {
  LeaseSpec spec;
  spec.model = "subscription";
  spec.major = 3;
  spec.extra["met"] = metClaim(meters);
  spec.extra["ent"] = {{"edition", edition}};
  StoredState state = *stored();
  state.lease = sign(spec);
  store(state);
}

json reported(MeterHarness& h, std::size_t nth) {
  const std::vector<json> bodies = h.http->bodiesOf("validate");
  if (nth >= bodies.size()) {
    ADD_FAILURE() << "expected validate request " << nth << ", there were " << bodies.size();
    return nullptr;
  }
  return bodies[nth].contains("spends") ? bodies[nth]["spends"] : json(nullptr);
}

void renewal(MeterHarness& h, const json& spends) {
  json body = {{"lease", meterLease(h)}};
  if (!spends.is_null()) body["spends"] = spends;
  h.http->reply("validate", 200, body);
}

MeterSpecs exportsOnly(std::int64_t left, std::int64_t total, std::int64_t offline) {
  return {{"exports", MeterSpec{left, total, offline, false}}};
}

const std::vector<std::string>& meteredGroups() {
  static const std::vector<std::string> groups = {"meterClaims",   "meterLeases",   "useAmounts", "meterFigures",
                                                  "meterStatuses", "offlineSpends", "useAsks",    "syncAsks",
                                                  "useOutcomes",   "journals",      "spendReports", "spendConfirmations"};
  return groups;
}

std::vector<SpendEntry> crowd(std::size_t count, const std::string& tag) {
  std::vector<SpendEntry> entries;
  for (std::size_t i = 0; i < count; ++i) {
    entries.push_back(SpendEntry{spendId(tag + std::to_string(i)), "m" + std::to_string(i), 1, NOW - 100, SpendKind::Unanswered});
  }
  return entries;
}

std::vector<std::string> ids(const std::vector<SpendEntry>& spends) {
  std::vector<std::string> out;
  for (const SpendEntry& e : spends) out.push_back(e.id);
  return out;
}

StoredState withBackoff(StoredState state, std::int64_t since, std::int64_t ms) {
  state.backoffSince = since;
  state.backoffMs = ms;
  return state;
}

StoredState journalState(const std::vector<SpendEntry>& spends) {
  StoredState state;
  state.activationId = "act_1";
  state.spends = spends;
  return state;
}

bool MergeStorage::update(const std::function<StoredState(const StoredState&)>& merge) {
  std::function<void(MemoryStorage&)> act;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++updates_;
    if (const auto hook = hooks_.find(updates_); hook != hooks_.end()) {
      act = hook->second;
      hooks_.erase(hook);
    }
  }
  if (act) act(*inner_);
  const StoredState latest = inner_->get().value_or(StoredState{});
  const StoredState next = merge(latest);
  if (failing) throw keygrant::IoError("EBUSY", "the disk said no");
  inner_->set(next);
  std::lock_guard<std::mutex> lock(mutex_);
  ++landed_;
  return true;
}

void MergeStorage::before(int nth, std::function<void(MemoryStorage&)> act) {
  std::lock_guard<std::mutex> lock(mutex_);
  hooks_[updates_ + nth] = std::move(act);
}

int MergeStorage::asked() {
  std::lock_guard<std::mutex> lock(mutex_);
  return updates_;
}

int MergeStorage::landed() {
  std::lock_guard<std::mutex> lock(mutex_);
  return landed_;
}

HarnessOptions mergingOptions(std::shared_ptr<MergeStorage>& out, HarnessOptions options) {
  options.wrapStorage = [&out](std::shared_ptr<MemoryStorage> inner) -> std::shared_ptr<keygrant::StorageAdapter> {
    out = std::make_shared<MergeStorage>(std::move(inner));
    return out;
  };
  return options;
}

}  // namespace kgtest
