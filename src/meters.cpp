#include "meters.hpp"

#include <algorithm>
#include <set>

#include "utf.hpp"

namespace keygrant::detail {

UseAsked askedFor(const std::string& meter, std::int64_t n) {
  UseAsked asked;
  if (n < 1 || n > kMaxMeterUses) {
    asked.refused = UseError::InvalidAmount;
    return asked;
  }
  // A UTF-16 unit takes at most 3 bytes of UTF-8 (a byte that is no UTF-8 reads as one unit), so more bytes than that
  // are too many units: said before the text is decoded.
  const std::size_t units = meter.size() > 3 * kMaxMeterCode ? kMaxMeterCode + 1 : utf16Length(meter);
  if (units < 1 || units > kMaxMeterCode) {
    asked.refused = UseError::NoMeter;
    return asked;
  }
  asked.meter = meter;
  asked.n = n;
  return asked;
}

BaseFigure baseOf(const MeterFigure& figure) { return BaseFigure{figure.left, figure.total, figure.offline, figure.resets}; }

std::int64_t unsyncedOf(const std::vector<SpendEntry>& spends, const std::string& meter) {
  std::int64_t sum = 0;
  for (const SpendEntry& e : spends) {
    if (e.kind != SpendKind::Void && e.meter == meter) sum = saturatingSum(sum, e.n);
  }
  return sum;
}

Meters figuresOf(const std::optional<Meters>& met, std::int64_t iat, const std::map<std::string, MeterFigure>& stored) {
  // From the lease's own second counts as newer: at worst a second's spends behind. A second so late that its
  // millisecond passes every time there is leaves nothing newer.
  const bool leaseFits = iat <= kMaxSafeInteger / 1000;
  const std::int64_t leaseMs = leaseFits ? iat * 1000 : 0;
  std::set<std::string> codes;
  if (met) {
    for (const auto& entry : *met) codes.insert(entry.first);
  }
  for (const auto& entry : stored) codes.insert(entry.first);
  Meters out;
  for (const std::string& code : codes) {
    const auto answered = stored.find(code);
    if (answered != stored.end() && leaseFits && answered->second.at >= leaseMs) {
      out.insert_or_assign(code, baseOf(answered->second));
      continue;
    }
    if (!met) continue;
    const auto leased = met->find(code);
    if (leased != met->end()) out.insert_or_assign(code, leased->second);
  }
  return out;
}

Meters figuresFor(const LeaseClaims& claims, const StoredState& state) {
  return figuresOf(claims.met, claims.iat, state.meterFigures);
}

std::int64_t leftOf(const LeaseClaims& claims, const StoredState& state, const std::string& meter) {
  const Meters figures = figuresFor(claims, state);
  const auto figure = figures.find(meter);
  if (figure == figures.end()) return 0;
  return std::max<std::int64_t>(0, figure->second.left - unsyncedOf(state.spends, meter));
}

std::map<std::string, MeterStatus> meterStatusOf(const Meters& figures, const std::vector<SpendEntry>& spends) {
  std::map<std::string, MeterStatus> out;
  for (const auto& [code, figure] : figures) {
    const std::int64_t unsynced = unsyncedOf(spends, code);
    out.insert_or_assign(code, MeterStatus{std::max<std::int64_t>(0, figure.left - unsynced), figure.total,
                                          figure.offline, unsynced, figure.resets});
  }
  return out;
}

LicenseStatus meteredStatus(LicenseStatus status, const LeaseClaims& claims, const StoredState& state) {
  if (status.state != LicenseState::Licensed) return status;
  status.meters = meterStatusOf(figuresFor(claims, state), state.spends);
  return status;
}

OfflineVerdict offlineSpend(const Meters& figures, const std::vector<SpendEntry>& spends, const std::string& meter,
                            std::int64_t n) {
  OfflineVerdict verdict;
  const auto found = figures.find(meter);
  if (found == figures.end()) return verdict;
  const BaseFigure& figure = found->second;
  const std::int64_t unsynced = unsyncedOf(spends, meter);
  const std::int64_t left = std::max<std::int64_t>(0, figure.left - unsynced);
  verdict.left = left;
  if (saturatingSum(unsynced, n) <= figure.offline && n <= left) {
    verdict.kind = OfflineVerdict::Kind::Allowed;
    verdict.left = left - n;
    verdict.limit = std::min(figure.offline, figure.left);
    return verdict;
  }
  verdict.kind = OfflineVerdict::Kind::OfflineLimit;
  return verdict;
}

bool journalWaiting(const std::vector<SpendEntry>& spends, std::int64_t now) {
  std::size_t count = 0;
  std::int64_t newest = 0;
  for (const SpendEntry& e : spends) {
    if (e.kind == SpendKind::Offline || e.at > now) continue;
    newest = count == 0 ? e.at : std::max(newest, e.at);
    ++count;
  }
  if (count == 0) return false;
  // The cap first: 2^(k-1) of a long journal overflows nothing, and from the seventh entry on it is the hour.
  const std::int64_t wait = count >= 7 ? kAskAgainMs : std::min(kAskAgainMs, kRetryUnansweredMs << (count - 1));
  return now < saturatingSum(newest, wait);
}

bool voidWaiting(const std::vector<SpendEntry>& spends, std::int64_t now, std::int64_t clockNow) {
  return std::any_of(spends.begin(), spends.end(), [&](const SpendEntry& e) {
    return e.kind == SpendKind::Void && e.at <= now && e.at <= clockNow && now < saturatingSum(e.at, kRetryUnansweredMs);
  });
}

bool useAsksNow(const StoredState& state, std::int64_t clockNow, std::int64_t now, bool covered) {
  if (!covered) return !voidWaiting(state.spends, now, clockNow);
  bool waiting = false;
  if (state.backoffSince && state.backoffMs == kRetryUnansweredMs) {
    waiting = clockNow >= *state.backoffSince && clockNow < saturatingSum(*state.backoffSince, kRetryUnansweredMs);
  }
  return !waiting && !journalWaiting(state.spends, now);
}

bool journalAsks(const std::vector<SpendEntry>& spends, std::int64_t now) {
  return !spends.empty() && !journalWaiting(spends, now);
}

}  // namespace keygrant::detail
