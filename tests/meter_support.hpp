// What the metered-uses tests share: journal entries and their JSON, a lease that carries meters, and a License over
// fake adapters whose key holds some.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine.hpp"
#include "journal.hpp"
#include "meters.hpp"
#include "support.hpp"

namespace kgtest {

using keygrant::MeterFigure;
using keygrant::MeterStatus;
using keygrant::SpendEntry;
using keygrant::SpendKind;
using keygrant::UseError;
using keygrant::UseResult;

/// The file `relative` in a folder above this one (or under that folder's `sdk` folder): the vectors, the captured bodies.
std::optional<std::filesystem::path> aboveTests(const std::string& relative);

/// Whether this is the repository that makes the vectors: it has the script that exports the SDKs.
bool inVectorsRepository();

/// A minute, an hour, in milliseconds.
inline constexpr std::int64_t MINUTE = 60'000;
inline constexpr std::int64_t HOUR = 3'600'000;

/// A spend's id as the tests write them: 22 characters of base64url that name `who` (one letter repeated, or a name padded).
std::string spendId(const std::string& who);

SpendEntry entry(SpendKind kind, const std::string& who, std::int64_t n, std::int64_t at, const std::string& meter = "exports");

/// An entry as the state file and the vectors write it.
json spendJson(const SpendEntry& e);
json spendsJson(const std::vector<SpendEntry>& spends);
/// A stored figure as the state file writes it.
json figureJson(const MeterFigure& f);
json figuresJson(const std::map<std::string, MeterFigure>& figures);
/// Status meters as the vectors write them.
json meterStatusJson(const std::map<std::string, MeterStatus>& meters);

/** One meter of a key as its lease signs it. */
struct MeterSpec {
  std::int64_t left = 90;
  std::int64_t total = 100;
  std::int64_t offline = 10;
  bool resets = false;
};
using MeterSpecs = std::map<std::string, MeterSpec>;

/// A key with 100 exports, 90 of them left and a device allowance of 10, and 5 renders a billing period with no allowance.
MeterSpecs defaultMeters();

/// A key with these exports and nothing else.
MeterSpecs exportsOnly(std::int64_t left, std::int64_t total, std::int64_t offline);

/// The twelve metered groups of the conformance vectors, in the file's order.
const std::vector<std::string>& meteredGroups();

/// A journal of `count` entries of other meters, each unanswered a moment ago.
std::vector<SpendEntry> crowd(std::size_t count, const std::string& tag = "c");

/// The ids of these entries, in order.
std::vector<std::string> ids(const std::vector<SpendEntry>& spends);

/// The state with a no-answer wait that began at `since` and lasts `ms` (by default a minute from NOW): a covered spend is made
/// without asking while it runs.
StoredState withBackoff(StoredState state, std::int64_t since = NOW, std::int64_t ms = keygrant::detail::kRetryUnansweredMs);

/// The `met` claim for these meters (`resets` only on a meter that has it).
json metClaim(const MeterSpecs& meters);

/** A key's lease for `act_1`, a subscription for majors 1 to 3 valid 30 days from `now`, with these meters. */
std::string meterLease(const Harness& h, const MeterSpecs& meters = defaultMeters(), std::int64_t now = NOW);

/** A lease with this JSON text as its `met` claim, signed by hand: the signer refuses what a verifier still reads. */
std::string rawMetLease(const Harness& h, const std::string& metText, std::int64_t now = NOW);

/** A state holding the key, `act_1` and a lease with meters, as the SDK would have stored them; `spends` beside them. */
StoredState meterState(const Harness& h, const MeterSpecs& meters = defaultMeters(),
                       const std::vector<SpendEntry>& spends = {}, std::int64_t now = NOW);

/** The server's answer to a spend that landed: what is left of `exports` now, at `at`. */
json spentBody(std::int64_t left, std::int64_t at = NOW);

/** A License on a build of major 1 whose key holds meters (defaultMeters unless given). */
class MeterHarness : public Harness {
 public:
  explicit MeterHarness(const MeterSpecs& meters = defaultMeters(), const std::vector<SpendEntry>& spends = {},
                        HarnessOptions options = {});

  /// The stored journal, and its shape as "unanswered:1 offline:3 void:1".
  std::vector<SpendEntry> journal();
  std::string shape();
  /// The `nth` `use` request's body (a test failure when there was none).
  json useBody(std::size_t nth = 0);
  /// Every `use` is answered with this.
  void answerUse(int status, const json& body) { http->reply("use", status, body); }
  /// Every `use` is a spend that landed, with `left` left of the exports.
  void answerSpent(std::int64_t left, std::int64_t at = NOW) { http->reply("use", 200, spentBody(left, at)); }
  UseResult use(const std::string& meter = "exports", std::int64_t n = 1) { return license().use(meter, n); }
  /// The stored journal's change of state as a status shows its meters.
  std::map<std::string, MeterStatus> meters();
  /// Signs the key's lease again with the entitlement `edition` beside these meters: a licence no meter may take away.
  void grantEdition(const std::string& edition = "pro", const MeterSpecs& meters = defaultMeters());
};

/// The nth `validate` request's report of spends, null when it carried none (a failure when there was no such request).
json reported(MeterHarness& h, std::size_t nth = 0);

/// A renewal's answer: the lease the harness's key signs for the key, and the confirmations the server names.
void renewal(MeterHarness& h, const json& spends = nullptr);

/** A state on act_1 whose journal is the given spends. */
StoredState journalState(const std::vector<SpendEntry>& spends);

/**
 * Storage that merges at the moment of saving, as the file storage's update() does, over the harness's in-memory one,
 * and lets a test act between a License's read and its save: another process's save, or a disk that refuses.
 */
class MergeStorage final : public keygrant::StorageAdapter {
 public:
  explicit MergeStorage(std::shared_ptr<MemoryStorage> inner) : inner_(std::move(inner)) {}
  std::optional<StoredState> read(bool readOnly) override { return inner_->read(readOnly); }
  void write(const StoredState& state) override { inner_->write(state); }
  bool update(const std::function<StoredState(const StoredState&)>& merge) override;

  /// Runs the given action just before the nth update from now merges (1 is the next); it may change the stored state (another
  /// process's save) or throw (a disk that refuses).
  void before(int nth, std::function<void(MemoryStorage&)> act);
  /// Every update fails after it merged, as a disk that refuses a write does (false to stop).
  std::atomic<bool> failing{false};
  /// How many updates were asked, and how many landed.
  int asked();
  int landed();

 private:
  std::shared_ptr<MemoryStorage> inner_;
  std::mutex mutex_;
  int updates_ = 0;
  int landed_ = 0;
  std::map<int, std::function<void(MemoryStorage&)>> hooks_;
};

/** Harness options that give the License a MergeStorage, kept in the given pointer. */
HarnessOptions mergingOptions(std::shared_ptr<MergeStorage>& out, HarnessOptions options = {});

}  // namespace kgtest
