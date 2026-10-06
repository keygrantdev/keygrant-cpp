// The clock guard (anti clock-rollback), decided without I/O (sdk/src/clock.ts):
// the time a lease is judged at, never earlier than the latest the device has
// seen (`guardedNow`); where a lease the server has just signed brings the
// guard back to after a clock that jumped ahead (`clockCorrection`, the
// engine's serverNow); and the latest time seen that a save writes
// (`savedLastSeen`, the engine's save).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace keygrant::detail {

/// How far the clock guard may run past a fresh lease's signing time before it is brought back.
inline constexpr std::int64_t kClockToleranceMs = 60 * 60'000;

/** Now, by the clock guard: never earlier than the latest time the device has seen. */
std::int64_t guardedNow(std::int64_t clockNow, const std::optional<std::int64_t>& lastSeen);

/** A fresh lease's claims as the clock guard reads them. */
struct SignedAt {
  std::string sub;
  std::string aid;
  /// Seconds.
  std::int64_t iat = 0;
};

/**
 * Where the clock guard should be brought back to on a lease the server has
 * just signed, or none to leave it: only for THIS key (without regard to case)
 * and activation, no older than the lease held (`heldIat`), and when the guard
 * is more than the tolerance past its signing time; to the device clock, never
 * below the signing time. `claims` is none when the lease did not verify.
 */
std::optional<std::int64_t> clockCorrection(std::int64_t clockNow, const std::optional<std::int64_t>& lastSeen,
                                            const std::optional<SignedAt>& claims,
                                            const std::optional<std::int64_t>& heldIat, const std::string& key,
                                            const std::string& activationId);

/**
 * The latest time seen (`lastSeen`) that a save writes onto the state as
 * stored when it saves (`stored`): the guard brought back on the server's
 * evidence (`guard`) exactly as given when the save carries one; otherwise
 * never earlier than what is stored, nor than the device clock now.
 */
std::int64_t savedLastSeen(const std::optional<std::int64_t>& stored, std::int64_t clockNow,
                           const std::optional<std::int64_t>& guard);

}  // namespace keygrant::detail
