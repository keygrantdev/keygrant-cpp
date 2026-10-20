// The machinery under License: the adapters, one call at a time, every request
// bounded, the stored state and its clock guard, the online validate and what
// its answer does to the stored state; the trial and its launch report; the
// journal of unsynced spends and the spending of a key's meters; and the
// decisions of when to go online and what to answer.
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
#include "journal.hpp"
#include "json_util.hpp"
#include "keygrant/license.hpp"
#include "lease.hpp"
#include "license_file.hpp"
#include "meters.hpp"
#include "offline.hpp"
#include "pickup.hpp"
#include "renewal.hpp"
#include "wire.hpp"

namespace keygrant::detail {

/** An online validate's outcome: the server's verdict (a status, or none), or no answer at all. */
struct Renewal {
  bool answered = false;
  std::optional<LicenseStatus> status;
};

/**
 * The stored state as this process sees it: the stored one with the journal changes it could not save landed on it.
 * `carried`: a held spend is in `state.spends`, so a request must not report them as they are (Engine::reportable).
 */
struct View {
  StoredState state;
  bool carried = false;

  /// The same view of `other`: what it carries is unchanged.
  View with(StoredState other) const { return View{std::move(other), carried}; }
};

class Engine {
 public:
  Engine(LicenseConfig config, const Timings& timings);

  ActivateResult activate(const std::string& key, const ActivateOptions& options);
  LicenseStatus status(bool forceOnline);
  /// Whether the stored licence grants `code`, offline (License::has).
  bool has(const std::string& code);
  DeactivateResult deactivate();
  /// Spend `n` uses of the key's meter `meter` (License::use).
  UseResult use(const std::string& meter, std::int64_t n);
  LicenseStatus startTrial();
  std::optional<std::string> upgradeUrl(const std::string& paymentLink);
  ImportLicenseFileResult importLicenseFile(std::string_view content, const ActivateOptions& options);
  void removeLicenseFile();
  /// Key pickup (License::pickUp, restoreByEmail): pickup.cpp holds the pure pieces, engine_pickup.cpp the calls.
  PickUpSentResult pickUp(const std::string& email);
  PickUpKeysResult confirmPickup(const std::string& email, const std::string& code);
  RestoreResult restoreByEmail(const std::string& email, const std::string& code);

  /**
   * Test support, not public API: whether the launch report in flight, if
   * any, settled within `timeout`.
   */
  bool launchReportSettled(std::chrono::milliseconds timeout);

  /**
   * Test support, not public API: the journal changes this process holds because a save failed, and the stored state as
   * this process sees it (the stored one with those landed on it), read without writing.
   */
  std::vector<JournalOp> heldChanges();
  std::optional<StoredState> seenState();

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

  /** Journal changes this process could not save: the disk refused them, or the call was behind one that has not finished. */
  struct Held {
    /// The changes, in the order they were made.
    std::vector<JournalOp> ops;
    /// Saves in flight: while one is, `ops` is only added to, never folded (keepUnsaved).
    int saving = 0;
  };

  /** One call to use(), as the steps of spend() read it. */
  struct Spending {
    /// The state the call read, with what this process could not save landed on it.
    View view;
    /// The claims of the lease that licenses this build.
    LeaseClaims claims;
    std::string meter;
    std::int64_t n = 0;
    /// The activation the spend is about.
    std::string about;
    /// The spend's id.
    std::string id;
    /// When (the guarded clock) the spend is made.
    std::int64_t at = 0;
    /// Behind a call that has not finished: nothing is saved.
    bool readOnly = false;
    /// What the offline allowance says of the spend as the call read the state.
    OfflineVerdict verdict;
  };

  /** What asking the server to spend came to: the answer it gave, or no usable answer (the spend is in doubt). */
  struct SpendAsk {
    std::optional<UseResult> answered;
  };

  /** The server's trial when it could not be saved: answered from, and saved by the next call that can. */
  struct UnsavedTrial {
    StatePatch patch;
    Basis basis;
  };

  /** The launch reports in flight, shared with each report's thread (launchReportSettled). */
  struct LaunchReports {
    std::mutex mutex;
    std::condition_variable settled;
    int inFlight = 0;
  };

  /**
   * One launch report, as its own thread holds it: everything the report
   * touches, by shared_ptr (the device reader, and through it the
   * fingerprinter; the HTTP adapter) or by value, and nothing of the engine,
   * so a License destroyed first leaves nothing it uses dangling.
   */
  struct LaunchReport {
    std::shared_ptr<DeviceIdentityReader> reader;
    std::shared_ptr<HttpAdapter> http;
    std::string url;
    DeviceBounds device;
    std::optional<DeviceKind> install;
    bool deviceHold = true;
    Milliseconds bound;
    std::optional<std::int64_t> trialStart;
  };

  // --- machinery (engine.cpp) ----------------------------------------------
  void serial(const std::function<void(bool readOnly)>& work);
  std::optional<Reply> post(const std::string& action, const ordered_json& body,
                            std::optional<Milliseconds> timeout = std::nullopt);
  std::string endpoint(const std::string& action) const;
  static std::optional<Reply> postTo(const std::shared_ptr<HttpAdapter>& http, const std::string& url,
                                     const ordered_json& body, Milliseconds bound);
  VerifyResult verify(const std::string& lease, std::int64_t now) const;
  LeaseCheck evaluate(const std::string& lease, std::int64_t now, const std::optional<std::string>& key,
                      const std::optional<std::string>& activationId, std::optional<DeviceKind> deviceKind);
  TrialLease trialLease(const std::string& lease, std::int64_t now, std::optional<DeviceKind> installKind);
  Device device(Milliseconds timeout, std::optional<DeviceKind> installKind);
  std::optional<DeviceKind> installKind(std::optional<DeviceKind> stored) const;
  StoredState stored(bool readOnly);
  View view(bool readOnly);
  StoredState load(bool readOnly = false);
  std::vector<SpendEntry> reportable(const View& view);
  void save(const StatePatch& patch, std::optional<std::int64_t> guard = std::nullopt, const Basis& basis = {});
  std::vector<bool> saveWith(const StatePatch& patch, std::optional<std::int64_t> guard, const Basis& basis,
                             const std::vector<JournalOp>& ops);
  std::vector<bool> saveOps(const StoredState& view, const StatePatch& patch, const Basis& basis,
                            const std::vector<JournalOp>& ops);
  std::vector<bool> keepUnsaved(const StoredState& view, const std::vector<JournalOp>& ops);
  void saveUnsavedJournal();
  bool saveBestEffort(const StatePatch& patch, std::optional<std::int64_t> guard = std::nullopt,
                      const Basis& basis = {});
  std::int64_t clockNow();
  std::int64_t effectiveNow(const StoredState& state);
  std::int64_t serverNow(const std::string& lease, const std::string& key, const std::string& activationId,
                         const GuardSave& save);
  std::optional<LicenseStatus> tryRenew(const View& view, std::optional<Milliseconds> timeout = std::nullopt);
  Renewal renew(const View& view, std::optional<Milliseconds> timeout);
  std::optional<LicenseStatus> metered(const std::optional<LicenseStatus>& status, const std::optional<LeaseCheck>& check,
                                       const StoredState& state, const std::vector<JournalOp>& ops);
  Judged judgeFresh(const StoredState& state, const std::optional<Reply>& response);
  Judged judged(const std::string& lease, const std::string& key, const std::string& activationId,
                std::optional<DeviceKind> kind, const std::function<std::int64_t()>& fallbackNow);
  /// What a validate and a use both send for the server's device check.
  ordered_json deviceBody(const StoredState& state, const Device& device) const;
  ordered_json validateBody(const StoredState& state, const Device& device,
                            const std::optional<ordered_json>& reports) const;
  /// The ids of the keys this build carries, as a check-in reports them (`kids`).
  ordered_json kidsJson() const;

  // --- decisions (license.cpp) ---------------------------------------------
  ActivateResult activateNow(const std::string& key, const ActivateOptions& options);
  DeactivateResult deactivateNow();
  bool release(const View& view, const Device& device);
  bool seatElsewhere(const StoredState& state, const Device& device);
  LicenseStatus statusNow(const View& view, bool forceOnline, bool readOnly);
  LicenseStatus startTrialNow(const StoredState& state);
  LicenseStatus refusedStatus(const View& view, Refusal refused, bool force);
  LicenseStatus licenseStatus(const View& view, const std::string& lease, std::int64_t now, bool forceOnline,
                              bool readOnly);
  View noteKept(const View& view, const LeaseClaims& claims, std::int64_t now);
  LicenseStatus trialStatus(const StoredState& state, std::int64_t now,
                            const std::optional<StatusAsk>& ask = std::nullopt);
  LicenseStatus trialOrFile(const StoredState& state, std::int64_t now, const std::optional<LicenseStatus>& file,
                            const StatusAsk& ask);
  std::optional<LicenseStatus> healTrial(const StoredState& state, bool forceOnline);
  TrialAsk askTrial(const StoredState& state, Milliseconds machineIdTimeout,
                    std::optional<Milliseconds> timeout = std::nullopt);
  bool keepsItsTrialLease(const StoredState& state, const std::optional<std::string>& answered);
  std::optional<View> loadForStatus(bool readOnly);
  void saveUnsavedTrial();

  // --- metered uses (spend.cpp) ---------------------------------------------
  UseResult spend(const std::string& meter, std::int64_t n, bool readOnly);
  std::optional<LeaseClaims> keyClaims(const View& view);
  SpendAsk askToSpend(const Spending& sp);
  UseResult spendOffline(const Spending& sp, bool doubt);
  std::int64_t landedLeft(const Spending& sp);
  std::vector<bool> journal(const Spending& sp, const std::vector<JournalOp>& ops);

  // --- licence files (license_files.cpp) ------------------------------------
  ImportLicenseFileResult importNow(std::string_view content, const ActivateOptions& options);
  LicenseFileCheck checkFile(const std::string& token) const;
  std::optional<LicenseStatus> fileStatus(const StoredState& state) const;

  // --- key pickup (engine_pickup.cpp) ---------------------------------------
  RestoreResult restoreNow(const std::string& email, const std::string& code);
  /// The keys a confirm with this address and code answered, while they are kept (kPickupKeysKeepMs).
  std::optional<std::vector<PickedKey>> keptKeys(const std::string& email, const std::string& code);

  // --- the launch report (trials.cpp) --------------------------------------
  void reportLaunch(const StoredState& state, std::int64_t now);
  static void sendLaunch(const LaunchReport& report);

  std::string product_;
  std::string base_;
  std::int64_t major_ = 1;
  std::optional<std::string> deviceName_;
  bool deviceHold_ = true;
  Milliseconds httpTimeout_;
  Timings timings_;
  KeySet keys_;
  /// The ids of licence files this build refuses (LicenseConfig::revokedLicenseFiles).
  std::vector<std::string> revoked_;
  Adapters a_;
  /// Shared with a launch report's thread, which may outlive the engine.
  std::shared_ptr<DeviceIdentityReader> reader_;

  std::mutex serialMutex_;
  std::condition_variable serialChanged_;
  std::shared_ptr<Slot> last_;
  int writing_ = 0;

  std::mutex unsavedMutex_;
  std::optional<UnsavedTrial> unsaved_;

  std::mutex heldMutex_;
  Held held_;

  /** The keys the last pickup confirm answered, with the address and code as sent and when: in memory only. */
  struct KeptKeys {
    std::string email;
    std::string code;
    std::vector<PickedKey> keys;
    std::int64_t at = 0;
  };
  std::mutex keptMutex_;
  std::optional<KeptKeys> kept_;

  std::shared_ptr<LaunchReports> reports_ = std::make_shared<LaunchReports>();
};

/** Test support, not public API: what the tests reach of a License's engine. */
class LicenseAccess {
 public:
  /// Engine::launchReportSettled of `license`'s engine.
  static bool launchReportSettled(const License& license, std::chrono::milliseconds timeout) {
    return license.engine_->launchReportSettled(timeout);
  }
  /// Engine::heldChanges of `license`'s engine.
  static std::vector<JournalOp> heldChanges(const License& license) { return license.engine_->heldChanges(); }
  /// Engine::seenState of `license`'s engine.
  static std::optional<StoredState> seenState(const License& license) { return license.engine_->seenState(); }
};

}  // namespace keygrant::detail
