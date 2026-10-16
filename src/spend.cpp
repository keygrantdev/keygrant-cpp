// Spending a key's meters under License: use() asks the server to spend (exact, atomic, idempotent by the spend's id),
// and when it gets no answer spends from the device's offline allowance, keeping the spend in the journal
// (engine_journal.cpp) to report with the next request.
//
// A meter never takes a licence away, and licensing fails in favour of the customer: no answer is never a refusal of the
// licence, only of a spend past the allowance, and the journal never fails a renewal.
#include <openssl/err.h>
#include <openssl/rand.h>

#include <algorithm>

#include "crypto.hpp"
#include "engine.hpp"

namespace keygrant::detail {

namespace {

/** A spend's id: 16 bytes from the platform's secure random source, base64url with no padding (22 characters), never reused. */
std::optional<std::string> newSpendId() {
  unsigned char bytes[16];
  if (RAND_bytes(bytes, sizeof bytes) != 1) {
    ERR_clear_error();  // the failure is answered below; it is not left on the app's OpenSSL error queue
    return std::nullopt;
  }
  return base64UrlEncode(bytes, sizeof bytes);
}

/** The error a server verdict is answered as. */
UseError useErrorOf(Refusal refusal) {
  switch (refusal) {
    case Refusal::Revoked: return UseError::Revoked;
    case Refusal::Upgrade: return UseError::Upgrade;
    case Refusal::Outdated: return UseError::Outdated;
    case Refusal::Device: return UseError::Device;
  }
  return UseError::Revoked;
}

/** The journal changes an answer makes: the reports it confirms and the figures it gives, about the activation asked for. */
std::vector<JournalOp> journalOps(const StoredState& state, const UseOutcome& outcome) {
  std::vector<JournalOp> ops;
  if (!state.activationId) return ops;
  if (!outcome.confirmed.empty()) ops.push_back(JournalOp::confirmedOf(*state.activationId, outcome.confirmed));
  if (!outcome.figures.empty()) ops.push_back(JournalOp::figuresOf(*state.activationId, outcome.figures));
  return ops;
}

/**
 * What use() answers for an answer the server gave. A spend: what is left of the meter by the figures as the answer left
 * them (`after`: the base figure, its own from the answer when it gave one, less what this device has not reported).
 * `used-up`: the answer's `left`. A verdict: the refusal, as status() will say it.
 */
UseResult answered(const UseOutcome& outcome, const std::string& meter, const LeaseClaims& claims, const StoredState& after) {
  switch (outcome.kind) {
    case UseOutcomeKind::Spent: return UseResult::spent(leftOf(claims, after, meter));
    case UseOutcomeKind::UsedUp: return UseResult::refused(UseError::UsedUp, outcome.left);
    case UseOutcomeKind::NoMeter: return UseResult::refused(UseError::NoMeter);
    case UseOutcomeKind::Refused:
    case UseOutcomeKind::Unanswered: break;
  }
  return UseResult::refused(outcome.refusal ? useErrorOf(*outcome.refusal) : UseError::NotLicensed);
}

}  // namespace

/**
 * Spend `n` of the meter `meter` (License::use). Never throws: what cannot be told is `storage`. Calls run one at a time
 * with the rest.
 */
UseResult Engine::use(const std::string& meter, std::int64_t n) {
  try {
    const UseAsked asked = askedFor(meter, n);
    if (asked.refused) return UseResult::refused(*asked.refused);
    UseResult result;
    serial([&](bool readOnly) { result = spend(asked.meter, asked.n, readOnly); });
    return result;
  } catch (...) {
    // Nothing an adapter does may fail a spend: the customer is told to try again shortly.
    return UseResult::refused(UseError::Storage);
  }
}

/** The steps of a spend: the state and the licence, then the server when it should be asked, then the allowance. */
UseResult Engine::spend(const std::string& meter, std::int64_t n, bool readOnly) {
  if (!readOnly) saveUnsavedJournal();
  std::optional<View> view = loadForStatus(readOnly);
  if (!view) return UseResult::refused(UseError::Storage);
  std::optional<LeaseClaims> claims = keyClaims(*view);
  if (!claims || !view->state.activationId) return UseResult::refused(UseError::NotLicensed);
  const std::int64_t at = effectiveNow(view->state);
  const OfflineVerdict verdict = offlineSpend(figuresFor(*claims, view->state), view->state.spends, meter, n);
  std::optional<std::string> id = newSpendId();
  if (!id) return UseResult::refused(UseError::Storage);
  Spending sp;
  sp.about = *view->state.activationId;
  sp.view = std::move(*view);
  sp.claims = std::move(*claims);
  sp.meter = meter;
  sp.n = n;
  sp.id = std::move(*id);
  sp.at = at;
  sp.readOnly = readOnly;
  sp.verdict = verdict;
  bool sent = false;
  if (useAsksNow(sp.view.state, clockNow(), at, verdict.allowed())) {
    SpendAsk ask = askToSpend(sp);
    if (ask.answered) return *ask.answered;
    sent = true;
  }
  return spendOffline(sp, sent);
}

/**
 * The claims of the key's lease when the state licenses this build here by a key's lease: what status() would give as
 * licensed from what is stored (no ask, no write), and not by a licence file. None otherwise.
 */
std::optional<LeaseClaims> Engine::keyClaims(const View& view) {
  const StoredState& state = view.state;
  const LicenseStatus status = statusNow(view, false, true);
  if (status.state != LicenseState::Licensed || status.source == LicenseSource::File || !truthy(state.lease)) {
    return std::nullopt;
  }
  const LeaseCheck check = evaluate(*state.lease, effectiveNow(state), state.key, state.activationId, state.deviceKind);
  if (!check.ok) return std::nullopt;
  return check.claims;
}

/**
 * Ask the server to spend (`use`, bounded by the status check's bound, 5 seconds, or the HTTP bound when shorter), with
 * the journal's report. Its answer is saved as a validate's is (useOutcome) unless this call is behind one that has not
 * finished; with no usable answer the spend is in doubt: the server may hold it.
 */
Engine::SpendAsk Engine::askToSpend(const Spending& sp) {
  const StoredState& state = sp.view.state;
  const std::int64_t askedAt = clockNow();
  const Device here = device(timings_.machineId, state.deviceKind);
  ordered_json body = deviceBody(state, here);
  body["meter"] = sp.meter;
  body["n"] = sp.n;
  body["id"] = sp.id;
  if (const std::optional<ordered_json> reports = spendsReported(reportable(sp.view))) body["spends"] = *reports;
  body["kids"] = kidsJson();
  const std::optional<Reply> response = post("use", body, std::min(timings_.statusCheck, httpTimeout_));
  UseInput input;
  input.response = response ? &*response : nullptr;
  input.askedAt = askedAt;
  input.major = major_;
  input.kind = here.kind;
  input.ownClaim = here.ownClaim;
  const UseOutcome outcome = useOutcome(input);
  const std::vector<JournalOp> ops = journalOps(state, outcome);
  // A verdict or a wait is the answer to an ask, stamped as a validate's is; figures and confirmations are not a verdict,
  // so a spend never makes a validate another process asked for earlier look out of date.
  const bool answer = !(outcome.patch == StatePatch{});
  if (!sp.readOnly && (!ops.empty() || answer)) {
    Basis basis;
    if (answer) basis.answer = Asked{sp.at, state.activationId, state.verdictAt};
    saveOps(state, outcome.patch, basis, ops);
  }
  SpendAsk ask;
  if (outcome.kind == UseOutcomeKind::Unanswered) return ask;
  ask.answered = answered(outcome, sp.meter, sp.claims, landJournal(state, ops, sp.at).state);
  return ask;
}

/**
 * With no answer (or none asked): spend from the allowance (offlineSpend). Allowed, the spend is journaled, `Offline` when
 * it was never sent and `Unanswered` when it was in doubt, with the most the meter's unsynced spends may come to
 * (`limit`), which the landing judges again on the state as stored then, so two processes never pass the allowance
 * together. Refused, a spend in doubt is journaled as a `Void`, for the server to take it back off if it recorded it, and
 * so is one the landing refuses (the journal full, or the allowance taken by another process meanwhile, answered with what
 * is left as the landing found it): the customer is told `OfflineLimit`, the server is told the spend is taken back. Voids
 * are kept to kMaxPendingVoids, a new one taking the place of the oldest, and a void a full journal cannot take is lost.
 */
UseResult Engine::spendOffline(const Spending& sp, bool doubt) {
  const auto entry = [&sp](SpendKind kind, std::optional<std::int64_t> limit) {
    return JournalOp::spendOf(kind, sp.about, sp.id, sp.meter, sp.n, sp.at, limit);
  };
  switch (sp.verdict.kind) {
    case OfflineVerdict::Kind::NoMeter:
      if (doubt) journal(sp, {entry(SpendKind::Void, std::nullopt)});
      return UseResult::refused(UseError::NoMeter);
    case OfflineVerdict::Kind::OfflineLimit:
      if (doubt) journal(sp, {entry(SpendKind::Void, std::nullopt)});
      return UseResult::refused(UseError::OfflineLimit, sp.verdict.left);
    case OfflineVerdict::Kind::Allowed: break;
  }
  const std::vector<bool> refused = journal(sp, {entry(doubt ? SpendKind::Unanswered : SpendKind::Offline, sp.verdict.limit)});
  if (!refused.empty() && refused.front()) {
    if (doubt) journal(sp, {entry(SpendKind::Void, std::nullopt)});
    return UseResult::refused(UseError::OfflineLimit, landedLeft(sp));
  }
  return UseResult::spent(sp.verdict.left, true);
}

/**
 * What is left of the meter by the state as a refused landing found it (another process had spent meanwhile): the figure
 * less the unsynced spends there then, never below 0, and 0 when the state cannot be read just now.
 */
std::int64_t Engine::landedLeft(const Spending& sp) {
  const std::optional<View> fresh = loadForStatus(sp.readOnly);
  return fresh ? leftOf(sp.claims, fresh->state, sp.meter) : 0;
}

/** `ops` saved, or kept in memory when this call may save nothing or the disk refuses. Whether each was refused. */
std::vector<bool> Engine::journal(const Spending& sp, const std::vector<JournalOp>& ops) {
  if (sp.readOnly) return keepUnsaved(sp.view.state, ops);
  return saveOps(sp.view.state, StatePatch{}, Basis{}, ops);
}

}  // namespace keygrant::detail
