#include "clock.hpp"

#include <algorithm>

#include "offline.hpp"

namespace keygrant::detail {

std::int64_t guardedNow(std::int64_t clockNow, const std::optional<std::int64_t>& lastSeen) {
  return std::max(clockNow, lastSeen.value_or(0));
}

std::optional<std::int64_t> clockCorrection(std::int64_t clockNow, const std::optional<std::int64_t>& lastSeen,
                                            const std::optional<SignedAt>& claims,
                                            const std::optional<std::int64_t>& heldIat, const std::string& key,
                                            const std::string& activationId) {
  if (!claims || !sameKey(claims->sub, key) || claims->aid != activationId) return std::nullopt;
  if (lastSeen.value_or(0) - claims->iat * 1000 <= kClockToleranceMs) return std::nullopt;
  if (heldIat && claims->iat < *heldIat) return std::nullopt;
  return std::max(clockNow, claims->iat * 1000);
}

std::int64_t savedLastSeen(const std::optional<std::int64_t>& stored, std::int64_t clockNow,
                           const std::optional<std::int64_t>& guard) {
  if (guard) return *guard;
  return std::max(stored.value_or(0), clockNow);
}

}  // namespace keygrant::detail
