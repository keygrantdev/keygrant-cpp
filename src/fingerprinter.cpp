// The default fingerprinter.
#include <mutex>
#include <stdexcept>

#include "crypto.hpp"
#include "keygrant/fingerprint.hpp"

namespace keygrant {

namespace {

/** A settled fingerprint, and what it hashes. */
struct Found {
  std::string fingerprint;
  DeviceKind kind = DeviceKind::Hostname;
};

/**
 * What a reading of the machine id settles on: its fingerprint; the host
 * name's (`hostFp`, read only then) when there is no id to read; none when the
 * read did not finish, which settles nothing.
 */
template <class HostFingerprint>
std::optional<Found> settledOn(const MachineIdReading& reading, HostFingerprint hostFp) {
  if (reading.kind == MachineIdReading::Kind::Id) return Found{machineFingerprint(reading.id), DeviceKind::Machine};
  if (reading.kind == MachineIdReading::Kind::Unavailable) return Found{hostFp(), DeviceKind::Hostname};
  return std::nullopt;
}

/**
 * Who a settled fingerprint is (none: none this time), with the host name's
 * (`legacy`) as an alternate when it is another: what the device may have
 * been activated under before.
 */
DeviceIdentity withHostAlternate(const std::optional<Found>& known, const std::string& legacy) {
  DeviceIdentity identity;
  if (!known) {
    // Not a stand-in: no fingerprint, and the host-name one only as what it may have been activated under.
    identity.alternates.push_back(legacy);
    return identity;
  }
  identity.fingerprint = known->fingerprint;
  identity.kind = known->kind;
  if (known->fingerprint != legacy) identity.alternates.push_back(legacy);
  return identity;
}

}  // namespace

std::string hashFingerprint(const std::string& raw) { return detail::sha256Hex("keygrant|" + raw); }

std::string machineFingerprint(const std::string& id) { return hashFingerprint("machine-id|" + id); }

std::string hostFingerprint(const std::string& host, const std::string& platform, const std::string& arch) {
  return hashFingerprint(host + "|" + platform + "|" + arch);
}

std::optional<HostNames> hostNames() {
  try {
    if (std::optional<std::string> host = hostName()) return HostNames{std::move(*host), platformName(), archName()};
  } catch (...) {
    // As Node's os.hostname() throwing.
  }
  return std::nullopt;
}

std::string hostNamesFingerprint(const std::optional<HostNames>& names) {
  return names ? hostFingerprint(names->host, names->platform, names->arch) : hashFingerprint(kUnknownHost);
}

std::string hostnameFingerprint() { return hostNamesFingerprint(hostNames()); }

DeviceIdentity identityOf(const MachineIdReading& reading, const std::optional<HostNames>& host) {
  const std::string legacy = hostNamesFingerprint(host);
  return withHostAlternate(settledOn(reading, [&legacy] { return legacy; }), legacy);
}

/** A settled fingerprint and what it hashes, kept once read. */
struct SystemFingerprinter::Settled {
  std::mutex mutex;
  std::optional<std::string> fingerprint;
  DeviceKind kind = DeviceKind::Hostname;
};

SystemFingerprinter::SystemFingerprinter() : source_(systemMachineIdSource()), own_(nullptr) {}

SystemFingerprinter::SystemFingerprinter(std::shared_ptr<MachineIdSource> source)
    : source_(std::move(source)), own_(std::make_shared<Settled>()) {}

std::optional<DeviceIdentity> SystemFingerprinter::identify(std::chrono::milliseconds timeout) {
  const std::shared_ptr<Settled> settled = own_ ? own_ : processWide();
  // As identityOf, with the settled fingerprint kept: the host name's
  // alternate is this call's, the fingerprint the one first settled on.
  const std::string legacy = hostnameFingerprint();
  std::optional<Found> known;
  {
    std::lock_guard<std::mutex> lock(settled->mutex);
    if (settled->fingerprint) known = Found{*settled->fingerprint, settled->kind};
  }
  if (!known) {
    known = settledOn(readMachineId(source_, timeout), [] { return hostnameFingerprint(); });
    if (known) {
      std::lock_guard<std::mutex> lock(settled->mutex);
      settled->fingerprint = known->fingerprint;
      settled->kind = known->kind;
    }
  }
  return withHostAlternate(known, legacy);
}

std::string SystemFingerprinter::get() {
  const std::optional<DeviceIdentity> identity = identify(kActivateMachineIdTimeout);
  if (!identity || !identity->fingerprint) throw std::runtime_error("this machine's id could not be read in time");
  return *identity->fingerprint;
}

std::shared_ptr<SystemFingerprinter::Settled> SystemFingerprinter::processWide() {
  // Never destroyed: an abandoned read may still settle it at exit.
  static auto* settled = new std::shared_ptr<Settled>(std::make_shared<Settled>());
  return *settled;
}

}  // namespace keygrant
