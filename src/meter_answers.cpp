// What a request reports of the journal, the reports an answer confirms, and what a `use` answer saves and says.
#include <algorithm>

#include "meters.hpp"

namespace keygrant::detail {

std::optional<ordered_json> spendsReported(const std::vector<SpendEntry>& spends) {
  ordered_json report = ordered_json::array();
  const std::size_t count = std::min(spends.size(), kMaxPendingSpends);
  for (std::size_t i = 0; i < count; ++i) {
    const SpendEntry& e = spends[i];
    ordered_json entry = ordered_json::object();
    entry["id"] = e.id;
    entry["meter"] = e.meter;
    entry["n"] = e.n;
    entry["at"] = e.at;
    if (e.kind == SpendKind::Void) entry["void"] = true;
    report.push_back(std::move(entry));
  }
  if (report.empty()) return std::nullopt;
  return report;
}

std::vector<Confirmed> spendsConfirmed(const Reply* response) {
  std::vector<Confirmed> confirmed;
  if (response == nullptr || noAnswer(response->status) || !response->body.is_object()) return confirmed;
  const auto listed = response->body.find("spends");
  if (listed == response->body.end() || !listed->is_array()) return confirmed;
  for (const json& member : *listed) {
    if (!member.is_object()) continue;
    const auto id = member.find("id");
    if (id == member.end() || !id->is_string()) continue;
    const auto voided = member.find("void");
    if (voided != member.end() && voided->is_boolean() && voided->get<bool>()) {
      confirmed.push_back(Confirmed::taken(id->get<std::string>()));
      continue;
    }
    const auto n = member.find("n");
    if (n == member.end()) continue;
    if (const std::optional<std::int64_t> count = countOf(*n)) confirmed.push_back(Confirmed::count(id->get<std::string>(), *count));
  }
  return confirmed;
}

const char* to_string(UseOutcomeKind kind) noexcept {
  switch (kind) {
    case UseOutcomeKind::Spent: return "spent";
    case UseOutcomeKind::UsedUp: return "used-up";
    case UseOutcomeKind::NoMeter: return "no-meter";
    case UseOutcomeKind::Refused: return "refused";
    case UseOutcomeKind::Unanswered: return "unanswered";
  }
  return "unanswered";
}

namespace {

/** An answer that saves and confirms nothing. */
UseOutcome nothing(UseOutcomeKind kind, StatePatch patch = {}) {
  UseOutcome outcome;
  outcome.kind = kind;
  outcome.patch = std::move(patch);
  return outcome;
}

/** The figures a `use` answer gives, each stamped with the server's time (`at`); none for an answer without a whole `at`. */
std::map<std::string, MeterFigure> answeredFigures(const json& body) {
  std::map<std::string, MeterFigure> figures;
  if (!body.is_object()) return figures;
  const auto atMember = body.find("at");
  if (atMember == body.end()) return figures;
  const std::optional<std::int64_t> at = figureOf(*atMember);
  if (!at) return figures;
  const auto meters = body.find("meters");
  if (meters == body.end()) return figures;
  for (const auto& [code, meter] : metersOf(*meters)) {
    figures.insert_or_assign(code, MeterFigure{meter.left, meter.total, meter.offline, meter.resets, *at});
  }
  return figures;
}

}  // namespace

UseOutcome useOutcome(const UseInput& input) {
  const StatePatch keyed = keyedBy(input.kind);
  if (input.response == nullptr) {
    StatePatch wait;
    wait.backoffSince = Change<std::int64_t>::set(input.askedAt);
    wait.backoffMs = Change<std::int64_t>::set(kRetryUnansweredMs);
    return nothing(UseOutcomeKind::Unanswered, wait + keyed);
  }
  const Reply& response = *input.response;
  if (noAnswer(response.status)) return nothing(UseOutcomeKind::Unanswered);
  const json& body = response.body;
  if (response.status == 200) {
    // A 200 is a spend only with `spent: true`.
    if (!trueMember(body, "spent")) return nothing(UseOutcomeKind::Unanswered);
    UseOutcome spent = nothing(UseOutcomeKind::Spent);
    spent.figures = answeredFigures(body);
    spent.confirmed = spendsConfirmed(&response);
    return spent;
  }
  const std::optional<std::string> error = errorWord(body);
  if (error == "used-up") {
    UseOutcome usedUp = nothing(UseOutcomeKind::UsedUp);
    const auto left = body.find("left");
    if (left != body.end()) usedUp.left = figureOf(*left);
    usedUp.figures = answeredFigures(body);
    usedUp.confirmed = spendsConfirmed(&response);
    return usedUp;
  }
  if (error == "unknown-meter") {
    UseOutcome unknown = nothing(UseOutcomeKind::NoMeter);
    unknown.confirmed = spendsConfirmed(&response);
    return unknown;
  }
  const std::optional<Refusal> refused = refusalOf(error);
  if (!refused) return nothing(UseOutcomeKind::Unanswered);
  StatePatch verdict;
  verdict.checkedMajor = Change<std::int64_t>::set(input.major);
  verdict.backoffSince = Change<std::int64_t>::set(input.askedAt);
  verdict.backoffMs = Change<std::int64_t>::set(kAskAgainMs);
  verdict += keyed;
  verdict += refusalPatch(*refused, input.major, input.ownClaim);
  UseOutcome outcome = nothing(UseOutcomeKind::Refused, std::move(verdict));
  outcome.refusal = refused;
  outcome.confirmed = spendsConfirmed(&response);
  return outcome;
}

}  // namespace keygrant::detail
