// The machinery under License (sdk/src/engine.ts and license.ts): the
// adapters, one call at a time, every request bounded, the stored state and
// its clock guard, the online validate and what its answer does to the stored
// state; and the decisions of when to go online and what to answer.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "clock.hpp"
#include "crypto.hpp"
#include "device.hpp"
#include "json_util.hpp"
#include "keygrant/license.hpp"
#include "lease.hpp"
#include "offline.hpp"
#include "renewal.hpp"
#include "wire.hpp"

namespace keygrant::detail {

/** An online validate's outcome: the server's verdict (a status, or none), or no answer at all. */
struct Renewal {
  bool answered = false;
  std::optional<LicenseStatus> status;
};

class Engine {
 public:
  Engine(LicenseConfig config, const Timings& timings);

  ActivateResult activate(const std::string& key);
  LicenseStatus status(bool forceOnline);
  DeactivateResult deactivate();
  LicenseStatus startTrial();
  std::optional<std::string> upgradeUrl(const std::string& paymentLink);

 private:
  using Milliseconds = std::chrono::milliseconds;
  using GuardSave = std::function<void(const StatePatch&, std::int64_t)>;

  /** The lease a validate's 200 carried, checked here (none for any other answer), and the time it was judged at. */
  struct Judged {
    std::optional<LeaseCheck> check;
    std::int64_t now = 0;
  };

  /** How a status() may ask: forced online, or read-only behind a call that has not settled. */
  struct StatusAsk {
    bool forceOnline = false;
    bool readOnly = false;
  };

  /** What asking the server for this device's trial gave: whether it answered at all, and the state with its trial. */
  struct TrialAsk {
    bool reached = false;
    StoredState latest;
  };

  /** One call's place in the order: when it started running, and whether it has settled. */
  struct Slot {
    bool started = false;
    bool settled = false;
    std::chrono::steady_clock::time_point graceOver;
  };

  /** The server's trial when it could not be saved: answered from, and saved by the next call that can. */
  struct UnsavedTrial {
    StatePatch patch;
    Basis basis;
  };

  // --- machinery (engine.cpp) ----------------------------------------------
  void serial(const std::function<void(bool readOnly)>& work);
  std::optional<Reply> post(const std::string& action, const ordered_json& body,
                            std::optional<Milliseconds> timeout = std::nullopt);
  VerifyResult verify(const std::string& lease, std::int64_t now) const;
  LeaseCheck evaluate(const std::string& lease, std::int64_t now, const std::optional<std::string>& key,
                      const std::optional<std::string>& activationId, std::optional<DeviceKind> deviceKind);
  TrialLease trialLease(const std::string& lease, std::int64_t now, std::optional<DeviceKind> installKind);
  Device device(Milliseconds timeout, std::optional<DeviceKind> installKind);
  std::optional<DeviceKind> installKind(std::optional<DeviceKind> stored) const;
  StoredState load(bool readOnly = false);
  void save(const StatePatch& patch, std::optional<std::int64_t> guard = std::nullopt, const Basis& basis = {});
  bool saveBestEffort(const StatePatch& patch, std::optional<std::int64_t> guard = std::nullopt,
                      const Basis& basis = {});
  std::int64_t clockNow();
  std::int64_t effectiveNow(const StoredState& state);
  std::int64_t serverNow(const std::string& lease, const std::string& key, const std::string& activationId,
                         const GuardSave& save);
  std::optional<LicenseStatus> tryRenew(const StoredState& state, std::optional<Milliseconds> timeout = std::nullopt);
  Renewal renew(const StoredState& state, std::optional<Milliseconds> timeout);
  Judged judgeFresh(const StoredState& state, const std::optional<Reply>& response);
  ordered_json validateBody(const StoredState& state, const Device& device) const;

  // --- decisions (license.cpp) ---------------------------------------------
  ActivateResult activateNow(const std::string& key);
  DeactivateResult deactivateNow();
  bool seatElsewhere(const StoredState& state);
  LicenseStatus statusNow(const StoredState& state, bool forceOnline, bool readOnly);
  LicenseStatus startTrialNow(const StoredState& state);
  LicenseStatus refusedStatus(const StoredState& state, Refusal refused, bool force);
  LicenseStatus licenseStatus(const StoredState& state, const std::string& lease, std::int64_t now, bool forceOnline,
                              bool readOnly);
  StoredState noteKept(const StoredState& state, const LeaseClaims& claims, std::int64_t now);
  LicenseStatus trialStatus(const StoredState& state, std::int64_t now,
                            const std::optional<StatusAsk>& ask = std::nullopt);
  std::optional<LicenseStatus> healTrial(const StoredState& state, bool forceOnline);
  TrialAsk askTrial(const StoredState& state, Milliseconds machineIdTimeout,
                    std::optional<Milliseconds> timeout = std::nullopt);
  bool keepsItsTrialLease(const StoredState& state, const std::optional<std::string>& answered);
  std::optional<StoredState> loadForStatus(bool readOnly);
  void saveUnsavedTrial();

  std::string product_;
  std::string base_;
  std::int64_t major_ = 1;
  std::optional<std::string> deviceName_;
  bool deviceHold_ = true;
  Milliseconds httpTimeout_;
  Timings timings_;
  VerifyingKey key_;
  Adapters a_;
  DeviceIdentityReader reader_;

  std::mutex serialMutex_;
  std::condition_variable serialChanged_;
  std::shared_ptr<Slot> last_;
  int writing_ = 0;

  std::mutex unsavedMutex_;
  std::optional<UnsavedTrial> unsaved_;
};

}  // namespace keygrant::detail
