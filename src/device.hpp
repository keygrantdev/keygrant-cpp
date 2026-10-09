// Who this device is, for holding a lease to it and telling the server: as far
// as can be told this call, and never guessed.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "bounded.hpp"
#include "keygrant/adapters.hpp"

namespace keygrant::detail {

/** Who this device is, as this call could tell. */
struct Device {
  /// This run's own fingerprint, or none when it cannot be told this time.
  std::optional<std::string> own;
  /// What `own` hashes; none when the fingerprinter does not say, or with no `own`.
  std::optional<DeviceKind> kind;
  bool canTell = false;
  /// Every fingerprint this device answers to: its own first, then its alternates.
  std::vector<std::string> fingerprints;
  /// Their `fp` claims.
  std::vector<std::string> claims;
  /// The claim of `own` alone: what a `device` refusal is held to.
  std::optional<std::string> ownClaim;
  /// The host-name fingerprint to activate under when the machine id cannot be read, or none.
  std::optional<std::string> hostname;
};

/** A fingerprint's `fp` claim (fingerprintClaim), which a reader may remember. */
using ClaimOf = std::function<std::string(const std::string& fingerprint)>;

/**
 * Who this device is, from what its fingerprinter said this time (`identity`),
 * on an install keyed as `installKind` (StoredState::deviceKind), held to its
 * device or not (`deviceHold`, LicenseConfig::deviceHold). `claimOf` is
 * fingerprintClaim, which a reader may remember.
 *
 * Install memory: once a machine id has keyed this install, a host-name answer
 * is not this device either. The id being unreadable now says nothing about
 * whether the machine changed, so it is "cannot tell", never the host name. A
 * fingerprint of no kind (a custom get()) is not judged by it.
 *
 * Install memory applies only to a held device. Not held (`deviceHold` false),
 * a device has no kind, so no seat is keyed to it, and is judged as a run that
 * cannot tell, so no lease is refused for its device and the server is told no
 * fingerprints. Its own still activates.
 */
Device deviceFrom(const DeviceIdentity& identity, std::optional<DeviceKind> installKind, bool deviceHold,
                  const ClaimOf& claimOf);

/** deviceFrom, with every claim computed (fingerprintClaim). */
Device deviceFrom(const DeviceIdentity& identity, std::optional<DeviceKind> installKind, bool deviceHold);

/** The fingerprint an activation is made under, and what it hashes. */
struct ActivationIdentity {
  std::string fingerprint;
  std::optional<DeviceKind> kind;
};

/**
 * The fingerprint an activation is made under (a typed key's, or a bought
 * key's claim), and what it hashes; or none: an activation answers `network`
 * for the app to retry, and a claim is not asked.
 *
 * The device's own, given the time an activation deserves. When even that
 * cannot be told: on an install already keyed by a machine id (`install`),
 * nothing (a seat under the host name would read as another device to every
 * normal run after it); otherwise the host name, so a machine whose id never
 * reads in time can still activate. A fingerprinter with no host-name
 * alternate (a custom get()) activates nothing.
 */
std::optional<ActivationIdentity> activationIdentity(const Device& here, std::optional<DeviceKind> install);

/** How long a read of the device may take. */
struct DeviceBounds {
  std::chrono::milliseconds timeout;
  /// What identify() is given beyond `timeout`.
  std::chrono::milliseconds identifyExtra;
};

/** Reads, and once settled keeps, who this device is. Safe to use from several threads. */
class DeviceIdentityReader {
 public:
  explicit DeviceIdentityReader(std::shared_ptr<Fingerprinter> fingerprinter);

  /**
   * Who this device is (deviceFrom), reading for at most about
   * `bounds.timeout`, on an install keyed as `installKind`, held to its device
   * unless `deviceHold` is false. A fingerprinter that throws, or does not
   * answer in time, gives "cannot tell", not kept: a failed read must neither
   * stand in for the device nor stick.
   *
   * One read of the fingerprinter at a time: a call that comes while one runs
   * waits on it (for its own bound) rather than start another beside it, so a
   * fingerprinter that never answers costs one thread, not one per call. An
   * identify() given less time than this call's is not joined.
   */
  Device read(DeviceBounds bounds, std::optional<DeviceKind> installKind, bool deviceHold);

 private:
  using Identifying = SharedCall<std::optional<DeviceIdentity>>;
  using Getting = SharedCall<std::string>;

  DeviceIdentity identity(DeviceBounds bounds);
  std::string claimOf(const std::string& fingerprint);

  std::shared_ptr<Fingerprinter> fingerprinter_;
  std::mutex mutex_;
  std::optional<DeviceIdentity> known_;
  std::map<std::string, std::string> claims_;
  /// The identify() in flight, and the time it was given.
  std::shared_ptr<Identifying> identifying_;
  std::chrono::milliseconds identifyingFor_{0};
  /// The get() in flight.
  std::shared_ptr<Getting> getting_;
};

}  // namespace keygrant::detail
