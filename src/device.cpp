#include "device.hpp"

#include <algorithm>

#include "bounded.hpp"
#include "crypto.hpp"
#include "utf.hpp"

namespace keygrant::detail {

namespace {

/** A fingerprint as the server will see it: well-formed UTF-8, as JSON carries it. */
DeviceIdentity wellFormed(DeviceIdentity identity) {
  if (identity.fingerprint) identity.fingerprint = sanitizeUtf8(*identity.fingerprint);
  for (std::string& alternate : identity.alternates) alternate = sanitizeUtf8(alternate);
  return identity;
}

}  // namespace

Device deviceFrom(const DeviceIdentity& identity, std::optional<DeviceKind> installKind, bool deviceHold,
                  const ClaimOf& claimOf) {
  // Install memory holds a held device only: not held, the install is keyed as nothing.
  const bool keyedByMachine = deviceHold && installKind == DeviceKind::Machine;
  // A kind with no fingerprint says nothing.
  std::optional<DeviceKind> read;
  if (identity.fingerprint) read = identity.kind;
  Device device;
  if (!(read == DeviceKind::Hostname && keyedByMachine)) device.own = identity.fingerprint;
  if (device.own) device.fingerprints.push_back(*device.own);
  for (const std::string& alternate : identity.alternates) {
    if (alternate != device.own) device.fingerprints.push_back(alternate);
  }
  if (read == DeviceKind::Hostname) {
    device.hostname = identity.fingerprint;
  } else if (!identity.alternates.empty()) {
    device.hostname = identity.alternates.front();
  }
  device.claims.reserve(device.fingerprints.size());
  for (const std::string& fingerprint : device.fingerprints) device.claims.push_back(claimOf(fingerprint));
  // Not held: judged as a run that cannot tell, with no kind and no claim of its own.
  if (!deviceHold) return device;
  device.canTell = device.own.has_value();
  if (device.own) {
    device.ownClaim = device.claims.front();
    device.kind = read;
  }
  return device;
}

Device deviceFrom(const DeviceIdentity& identity, std::optional<DeviceKind> installKind, bool deviceHold) {
  return deviceFrom(identity, installKind, deviceHold, [](const std::string& f) { return fingerprintClaim(f); });
}

std::optional<ActivationIdentity> activationIdentity(const Device& here, std::optional<DeviceKind> install) {
  if (here.own) return ActivationIdentity{*here.own, here.kind};
  if (install == DeviceKind::Machine || !here.hostname) return std::nullopt;
  return ActivationIdentity{*here.hostname, DeviceKind::Hostname};
}

DeviceIdentityReader::DeviceIdentityReader(std::shared_ptr<Fingerprinter> fingerprinter)
    : fingerprinter_(std::move(fingerprinter)) {}

Device DeviceIdentityReader::read(DeviceBounds bounds, std::optional<DeviceKind> installKind, bool deviceHold) {
  std::optional<DeviceIdentity> settled;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    settled = known_;
  }
  const DeviceIdentity found = settled ? *settled : identity(bounds);
  if (found.fingerprint) {
    std::lock_guard<std::mutex> lock(mutex_);
    known_ = found;
  }
  return deviceFrom(found, installKind, deviceHold, [this](const std::string& f) { return claimOf(f); });
}

DeviceIdentity DeviceIdentityReader::identity(DeviceBounds bounds) {
  const std::shared_ptr<Fingerprinter> fingerprinter = fingerprinter_;
  const std::chrono::milliseconds timeout = bounds.timeout;
  // identify(), when the fingerprinter has it, given a little longer than its
  // own bound. The one in flight is joined when it was given at least as long.
  std::shared_ptr<Identifying> identifying;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!identifying_ || identifying_->finished() || identifyingFor_ < timeout) {
      identifying_ = Identifying::start([fingerprinter, timeout] { return fingerprinter->identify(timeout); });
      identifyingFor_ = timeout;
    }
    identifying = identifying_;
  }
  const auto identified = identifying->wait(timeout + bounds.identifyExtra);
  if (!identified) return DeviceIdentity{};
  if (*identified) return wellFormed(**identified);
  // get() alone: nothing said of what it hashes.
  std::shared_ptr<Getting> getting;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!getting_ || getting_->finished()) getting_ = Getting::start([fingerprinter] { return fingerprinter->get(); });
    getting = getting_;
  }
  const auto fingerprint = getting->wait(timeout);
  if (!fingerprint) return DeviceIdentity{};
  DeviceIdentity got;
  got.fingerprint = sanitizeUtf8(*fingerprint);
  return got;
}

/** A fingerprint's claim (fingerprintClaim), remembered. */
std::string DeviceIdentityReader::claimOf(const std::string& fingerprint) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = claims_.find(fingerprint);
  if (it == claims_.end()) it = claims_.emplace(fingerprint, fingerprintClaim(fingerprint)).first;
  return it->second;
}

}  // namespace keygrant::detail
