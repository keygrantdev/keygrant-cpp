// A lease held to its key, its activation and its device, and who the device is when it cannot be
// told.
#include <thread>

#include "machine_id.hpp"
#include "support.hpp"

namespace kgtest {
namespace {

using namespace std::chrono_literals;
using keygrant::hashFingerprint;
using keygrant::hostnameFingerprint;

constexpr const char* kGuid = "3f2504e0-4f89-11d3-9a0c-0305e82c3301";
const std::string MACHINE = keygrant::machineFingerprint(kGuid);

/** A Windows machine whose registry read never finishes the first `hangs` times (until released), then answers. */
class SlowRegistry final : public keygrant::MachineIdSource {
 public:
  explicit SlowRegistry(int hangs, bool blocked = false) : hangs_(hangs), blocked_(blocked) {
    for (const char* c = kGuid; *c != 0; ++c) guid_.push_back(static_cast<char16_t>(*c));
  }
  std::string platform() override { return "win32"; }
  keygrant::RegistryRead readMachineGuid() override {
    // Touches nothing but this object: a read abandoned at its bound may finish after the test.
    const InFlight counted(inFlight_);
    // Blocked: the value cannot be read, for good (an endpoint tool, say).
    if (blocked_) return {keygrant::detail::kWinAccessDenied, {}};
    if (hangs_.fetch_sub(1) > 0) gate->wait();
    return {keygrant::detail::kWinSuccess, guid_};
  }
  /// Lets every held read go, and waits for them to finish.
  void release() {
    gate->open();
    for (int i = 0; i < 500 && inFlight_.load() > 0; ++i) std::this_thread::sleep_for(Millis{10});
  }
  keygrant::IoKitRead readPlatformUuid() override { return {keygrant::IoKitRead::Status::NoService, {}}; }
  std::string readFile(const std::string& path) override { throw keygrant::IoError("ENOENT", path); }
  std::optional<std::int64_t> mtimeMs(const std::string&) override { return std::nullopt; }

  std::shared_ptr<Gate> gate = std::make_shared<Gate>();

 private:
  /** Counts a read in flight for as long as it runs. */
  struct InFlight {
    explicit InFlight(std::atomic<int>& n) : count(n) { ++count; }
    ~InFlight() { --count; }
    std::atomic<int>& count;
  };
  std::atomic<int> hangs_;
  bool blocked_;
  std::u16string guid_;
  std::atomic<int> inFlight_{0};
};

class Device : public ::testing::Test {
 protected:
  void TearDown() override {
    for (const auto& source : sources_) source->release();
  }

  /** The default fingerprinter over a scripted registry. */
  std::shared_ptr<keygrant::Fingerprinter> registry(int hangs, bool blocked = false) {
    auto source = std::make_shared<SlowRegistry>(hangs, blocked);
    sources_.push_back(source);
    return std::make_shared<keygrant::SystemFingerprinter>(source);
  }

  static HarnessOptions with(std::shared_ptr<keygrant::Fingerprinter> fingerprint, std::optional<bool> deviceHold = std::nullopt) {
    HarnessOptions o;
    o.fingerprint = std::move(fingerprint);
    o.timings = shortTimings();
    o.deviceHold = deviceHold;
    return o;
  }

  /** A 30-day licence lease, its seat keyed (`kind`) by the fingerprint `device`, or held to no device. */
  static std::string leaseFor(const Harness& h, const std::optional<std::string>& device, const char* kind = "machine",
                              const std::string& sub = KEY, const std::string& aid = "act_1") {
    LeaseSpec spec;
    spec.sub = sub;
    spec.aid = aid;
    if (device) spec.extra = {{"fp", claimOf(*device)}, {"fpk", kind}};
    return h.sign(spec);
  }

  static json leaseAnswer(const std::string& lease, const std::string& activationId = "act_1") {
    return {{"lease", lease}, {"activationId", activationId}};
  }

 protected:
  std::vector<std::shared_ptr<SlowRegistry>> sources_;
};

TEST_F(Device, LicensesThisDeviceOnItsOwnLeaseOffline) {
  Harness h;
  h.store(held(leaseFor(h, std::string("fp-test"))));
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.http->count("validate"), 0);
}

TEST_F(Device, TakesALeaseWithNoDeviceClaimAndMatchesTheKeyWithoutRegardToCase) {
  Harness before;
  before.store(held(leaseFor(before, std::nullopt)));
  before.http->fail = true;
  EXPECT_EQ(before.license().status().state, LicenseState::Licensed);
  Harness cased;
  cased.store(held(leaseFor(cased, std::string("fp-test"), "machine", "Sluice-aaaa-BBBB-CCCC-DDDD")));
  cased.http->fail = true;
  EXPECT_EQ(cased.license().status().state, LicenseState::Licensed);
}

TEST_F(Device, RefusesOfflineALeaseForAnotherKeyActivationOrDeviceAndAsksUnderTheUsualWaits) {
  struct Case {
    const char* name;
    std::string sub;
    std::string aid;
    std::string device;
    LicenseStatus refused;
  };
  const std::vector<Case> cases = {
      {"another key", "SLUICE-OTHR-OTHR-OTHR-OTHR", "act_1", "fp-test", status(LicenseState::Invalid, LicenseReason::Tampered)},
      {"another activation", KEY, "act_2", "fp-test", status(LicenseState::Invalid, LicenseReason::Tampered)},
      {"another device", KEY, "act_1", "fp-another-machine", status(LicenseState::Invalid, LicenseReason::Device, KEY)},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    Harness h;
    h.store(held(leaseFor(h, c.device, "machine", c.sub, c.aid)));
    h.http->fail = true;
    EXPECT_EQ(h.license().status(), c.refused);
    EXPECT_EQ(h.http->count("validate"), 1);
    EXPECT_EQ(h.license().status(), c.refused);
    EXPECT_EQ(h.http->count("validate"), 1);
  }
}

TEST_F(Device, StaysRefusedForAnotherDeviceEvenOnlineTheServerRenewsTheOtherDevicesLease) {
  Harness h;
  const std::string theirs = leaseFor(h, std::string("fp-another-machine"));
  h.store(held(theirs));
  h.http->reply("validate", 200, leaseAnswer(theirs));
  const LicenseStatus device = status(LicenseState::Invalid, LicenseReason::Device, KEY);
  EXPECT_EQ(h.license().status(), device);
  EXPECT_EQ(h.license().refresh(), device);
}

TEST_F(Device, SaysDeviceOfflineTooOnceALeaseWithNoDeviceClaimIsRenewedWithAnotherDevices) {
  Harness h;
  h.store(held(leaseFor(h, std::nullopt)));
  h.http->reply("validate", 200, leaseAnswer(leaseFor(h, std::string("fp-another-machine"))));
  const LicenseStatus device = status(LicenseState::Invalid, LicenseReason::Device, KEY);
  EXPECT_EQ(h.license().refresh(), device);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), device);
}

TEST_F(Device, AcceptsTheLeaseAnActivationOfThisDeviceIsGivenAndRefusesOneForAnother) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(leaseFor(h, std::string("fp-test"))));
  EXPECT_EQ(h.license().activate(KEY), ok());
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
  Harness other;
  other.http->reply("activate", 200, leaseAnswer(leaseFor(other, std::string("fp-another-machine"))));
  EXPECT_EQ(other.license().activate(KEY), failed(ActivateError::InvalidLease));
}

TEST_F(Device, DoesNotCallASlowFirstReadAnotherDeviceNoFlipThenTheId) {
  const auto source = std::make_shared<SlowRegistry>(1);
  sources_.push_back(source);
  Harness first(std::nullopt, with(std::make_shared<keygrant::SystemFingerprinter>(source)));
  first.store(held(leaseFor(first, MACHINE)));
  first.http->fail = true;
  // The read outlives its time box: this run cannot tell, and does not judge.
  EXPECT_EQ(first.license().status(), licensed(KEY));
  // The slow read ends; the next run reads the id and judges by it.
  source->release();
  Harness second(std::nullopt, with(std::make_shared<keygrant::SystemFingerprinter>(source)));
  second.store(held(leaseFor(second, MACHINE)));
  second.http->fail = true;
  EXPECT_EQ(second.license().status(), licensed(KEY));
}

TEST_F(Device, ActivatesUnderTheHostNameWhenTheIdNeverReadsInTimeAfterTheActivationsLongerBound) {
  Harness h(std::nullopt, with(registry(1)));
  h.http->reply("activate", 200, leaseAnswer(leaseFor(h, hostnameFingerprint(), "hostname")));
  ActivateResult result;
  const Millis took = timed([&] { result = h.license().activate(KEY); });
  EXPECT_EQ(result, ok());
  // The activation's bound (400 ms here, 10 s by default) plus the read's slack, not a status()'s.
  EXPECT_GE(took, Millis{850});
  EXPECT_EQ(h.http->bodyOf("activate")["fingerprint"], hostnameFingerprint());
  EXPECT_EQ(h.http->bodyOf("activate")["fingerprintKind"], "hostname");
  EXPECT_EQ(h.stored()->deviceKind, DeviceKind::Hostname);
  // The slow read ends and the id reads now: the renewal sends it first, as a
  // machine id, and the install is keyed by it.
  sources_.back()->release();
  h.http->reply("validate", 200, leaseAnswer(leaseFor(h, MACHINE)));
  EXPECT_EQ(h.license().refresh(), licensed(KEY));
  EXPECT_EQ(h.http->bodyOf("validate")["fingerprints"], json({MACHINE, hostnameFingerprint()}));
  EXPECT_EQ(h.http->bodyOf("validate")["fingerprintKind"], "machine");
  EXPECT_EQ(h.stored()->deviceKind, DeviceKind::Machine);
}

TEST_F(Device, NeverReactivatesUnderTheHostNameTheSeatAMachineIdKeyed) {
  StoredState state;
  state.key = KEY;
  state.activationId = "act_1";
  state.deviceKind = DeviceKind::Machine;
  state.lastSeen = NOW;
  for (const char* stored : {KEY, "sluice-aaaa-bbbb-cccc-dddd"}) {
    state.key = stored;
    Harness h(state, with(registry(1)));
    EXPECT_EQ(h.license().activate(KEY), failed(ActivateError::Network)) << stored;
    EXPECT_FALSE(h.http->called("activate"));
  }
}

TEST_F(Device, TakesALeaseFromAnActivationUnderTheHostNameAndRenewsIt) {
  Harness h(std::nullopt, with(registry(0)));
  const std::string legacy = leaseFor(h, hostnameFingerprint(), "hostname");
  h.store(held(legacy));
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
  h.http->fail = false;
  h.http->reply("validate", 200, leaseAnswer(legacy));
  EXPECT_EQ(h.license().refresh(), licensed(KEY));
  EXPECT_EQ(h.http->bodyOf("validate")["fingerprints"], json({MACHINE, hostnameFingerprint()}));
}

TEST_F(Device, NeitherJudgesTheDeviceNorTellsTheServerWhoItIsOnARunThatCannotTell) {
  auto failing = std::make_shared<FakeFingerprinter>();
  failing->failing = true;
  Harness h(std::nullopt, with(failing));
  const std::string elsewhere = leaseFor(h, std::string("fp-another-machine"));
  h.store(held(elsewhere));
  EXPECT_EQ(h.license().status(), licensed(KEY));
  h.http->reply("validate", 200, leaseAnswer(elsewhere));
  EXPECT_EQ(h.license().refresh(), licensed(KEY));
  EXPECT_FALSE(h.http->bodyOf("validate").contains("fingerprints"));
  // Not kept: every call asks the adapter again.
  EXPECT_GT(failing->calls, 1);
}

TEST_F(Device, AFingerprinterThatNeverAnswersIsAskedOnceHoweverManyCallsWaitOnIt) {
  // A hung fingerprinter costs one thread, not one more on every status().
  auto hung = std::make_shared<FakeFingerprinter>();
  hung->hanging = true;
  Harness h(std::nullopt, with(hung));
  h.store(held(leaseFor(h, std::string("fp-test"))));
  h.http->fail = true;
  for (int i = 0; i < 4; ++i) EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(hung->calls.load(), 1);
  // Once it ends, the next call asks afresh.
  hung->hanging = false;
  hung->gate().open();
  for (int i = 0; i < 100 && hung->calls.load() < 2; ++i) {
    h.license().status();
    if (hung->calls.load() < 2) std::this_thread::sleep_for(Millis{10});
  }
  EXPECT_EQ(hung->calls.load(), 2);
}

TEST_F(Device, ReadsAgainAfterAnAdapterThatFailedAndJudgesTheDeviceOnceItCanTell) {
  auto flaky = std::make_shared<FakeFingerprinter>();
  flaky->failing = true;
  Harness h(std::nullopt, with(flaky));
  h.store(held(leaseFor(h, std::string("fp-another-machine"))));
  h.http->fail = true;
  EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
  flaky->failing = false;
  EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, LicenseReason::Device, KEY));
}

TEST_F(Device, TakesTheServersDeviceRefusalForThisDeviceOnlyKeepingTheLease) {
  Harness h;
  h.store(held(leaseFor(h, std::string("fp-test"))));
  h.http->reply("validate", 403, {{"error", "device"}});
  const LicenseStatus device = status(LicenseState::Invalid, LicenseReason::Device, KEY);
  EXPECT_EQ(h.license().refresh(), device);
  EXPECT_EQ(h.http->bodyOf("validate")["fingerprints"], json({"fp-test"}));
  // A custom get() says nothing of what it hashes: no kind is claimed for it.
  EXPECT_FALSE(h.http->bodyOf("validate").contains("fingerprintKind"));
  EXPECT_EQ(h.stored()->refused, keygrant::Refusal::Device);
  EXPECT_EQ(h.stored()->refusedFor, std::vector<std::string>{claimOf("fp-test")});
  EXPECT_TRUE(h.stored()->lease);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), device);
}

TEST_F(Device, LicensesAgainAtOnceWhenTheFingerprintFlipsBackAfterADeviceRefusal) {
  auto flipping = std::make_shared<FakeFingerprinter>("fp-flipped");
  Harness h(std::nullopt, with(flipping));
  h.store(held(leaseFor(h, std::string("fp-test"))));
  h.http->reply("validate", 403, {{"error", "device"}});
  EXPECT_EQ(h.license().refresh().reason, LicenseReason::Device);
  flipping->set("fp-test");
  const auto next = h.another(flipping);
  h.http->fail = true;
  EXPECT_EQ(next->status(), licensed(KEY));
}

TEST_F(Device, DoesNotStandADeviceRefusalOnALaterRunThatOnlySharesItsHostNameAlternate) {
  auto booting = std::make_shared<FakeFingerprinter>("fp-boot-1");
  booting->identifyAs(DeviceKind::Machine, {"fp-same-host"});
  Harness h(std::nullopt, with(booting));
  // A lease whose seat's kind is unknown: not itself judged by its device.
  h.store(held(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  h.http->reply("validate", 403, {{"error", "device"}});
  EXPECT_EQ(h.license().refresh().reason, LicenseReason::Device);
  EXPECT_EQ(h.stored()->refusedFor, std::vector<std::string>{claimOf("fp-boot-1")});
  booting->set("fp-boot-2");
  const auto next = h.another(booting);
  h.http->fail = true;
  EXPECT_EQ(next->status(), licensed(KEY));
}

TEST_F(Device, DoesNotRefuseAMachineKeyedInstallWhoseIdStopsReadingAndKeepsItsLease) {
  Harness h(std::nullopt, with(registry(0, true)));
  const std::string lease = leaseFor(h, MACHINE);
  StoredState state = held(lease);
  state.deviceKind = DeviceKind::Machine;
  h.store(state);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
  h.http->fail = false;
  h.http->reply("validate", 200, leaseAnswer(lease));
  EXPECT_EQ(h.license().refresh(), licensed(KEY));
  EXPECT_FALSE(h.http->bodyOf("validate").contains("fingerprints"));
  EXPECT_EQ(h.stored()->lease, lease);
  EXPECT_EQ(h.stored()->deviceKind, DeviceKind::Machine);
}

TEST_F(Device, DoesNotRefuseAHostNameKeyedSeatRenamedWithOrWithoutAnIdToRead) {
  for (const bool blocked : {false, true}) {
    Harness h(std::nullopt, with(registry(0, blocked)));
    h.store(held(leaseFor(h, hashFingerprint("old-name|darwin|x64"), "hostname")));
    h.http->fail = true;
    EXPECT_EQ(h.license().status(), licensed(KEY)) << blocked;
  }
}

TEST_F(Device, DoesNotJudgeALeaseWhoseSeatsKindIsUnknown) {
  Harness h(std::nullopt, with(registry(0)));
  LeaseSpec spec;
  spec.extra = {{"fp", claimOf(hashFingerprint("old-name|darwin|x64"))}};
  h.store(held(h.sign(spec)));
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
}

TEST_F(Device, HoldsNothingToItsDeviceWithDeviceHoldFalse) {
  // A product on non-persistent, pooled VDI: every session may be another machine.
  Harness h(std::nullopt, with(registry(0), false));
  h.http->reply("activate", 200, leaseAnswer(leaseFor(h, MACHINE)));
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.http->bodyOf("activate")["fingerprint"], MACHINE);
  EXPECT_FALSE(h.http->bodyOf("activate").contains("fingerprintKind"));
  EXPECT_EQ(h.stored()->deviceKind, std::nullopt);
  const std::string elsewhere = leaseFor(h, std::string("fp-another-machine"));
  h.store(held(elsewhere));
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
  h.http->fail = false;
  h.http->reply("validate", 200, leaseAnswer(elsewhere));
  EXPECT_EQ(h.license().refresh(), licensed(KEY));
  EXPECT_FALSE(h.http->bodyOf("validate").contains("fingerprints"));
  EXPECT_FALSE(h.http->bodyOf("validate").contains("fingerprintKind"));
}

TEST_F(Device, ActivatesUnderTheHostNameWithDeviceHoldFalseOnAnInstallAMachineIdOnceKeyed) {
  StoredState state;
  state.key = KEY;
  state.activationId = "act_1";
  state.deviceKind = DeviceKind::Machine;
  state.lastSeen = NOW;
  Harness h(state, with(registry(0, true), false));
  h.http->reply("activate", 200, leaseAnswer(leaseFor(h, MACHINE)));
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.http->bodyOf("activate")["fingerprint"], hostnameFingerprint());
  EXPECT_FALSE(h.http->bodyOf("activate").contains("fingerprintKind"));
  EXPECT_EQ(h.stored()->deviceKind, std::nullopt);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
}

TEST_F(Device, ClearsAStoredMachineKindWhenAFingerprintOfUnknownKindActivatesTheSeat) {
  StoredState state;
  state.key = KEY;
  state.activationId = "act_1";
  state.deviceKind = DeviceKind::Machine;
  state.lastSeen = NOW;
  Harness h(state);
  h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.stored()->deviceKind, std::nullopt);
}

TEST_F(Device, ActivatesUnderTheHostNameOnceADeactivateFreesAMachineInstallWhoseIdNoLongerReads) {
  Harness h(std::nullopt, with(registry(0, true)));
  StoredState state = held(leaseFor(h, MACHINE));
  state.deviceKind = DeviceKind::Machine;
  h.store(state);
  h.http->reply("deactivate", 200, {{"ok", true}});
  h.license().deactivate();
  EXPECT_EQ(h.stored()->deviceKind, std::nullopt);
  h.http->reply("activate", 200, leaseAnswer(leaseFor(h, hostnameFingerprint(), "hostname")));
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.http->bodyOf("activate")["fingerprint"], hostnameFingerprint());
  EXPECT_EQ(h.http->bodyOf("activate")["fingerprintKind"], "hostname");
  EXPECT_EQ(h.stored()->deviceKind, DeviceKind::Hostname);
}

TEST_F(Device, ActivatesAnotherKeyUnderTheHostNameOnAMachineInstallWhoseIdNoLongerReads) {
  StoredState state;
  state.key = "SLUICE-OLDK-OLDK-OLDK-OLDK";
  state.activationId = "act_1";
  state.deviceKind = DeviceKind::Machine;
  state.lastSeen = NOW;
  Harness h(state, with(registry(0, true)));
  h.http->reply("activate", 200, leaseAnswer(leaseFor(h, hostnameFingerprint(), "hostname")));
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.http->bodyOf("activate")["fingerprint"], hostnameFingerprint());
  EXPECT_EQ(h.http->bodyOf("activate")["fingerprintKind"], "hostname");
}

TEST_F(Device, TakesATrialLeaseStartedUnderTheHostNameItAnswersToBesidesItsMachineId) {
  Harness h(std::nullopt, with(registry(0)));
  const std::string claim = claimOf(hostnameFingerprint());
  LeaseSpec spec;
  spec.sub = "trial:" + claim;
  spec.aid = claim;
  spec.model = "trial";
  spec.lim = 1;
  spec.major = keygrant::kOpenMajor;
  spec.ttlSeconds = 7 * 24 * 3600;
  spec.extra = {{"fpk", "machine"}};
  StoredState state;
  state.trialStart = NOW - 2 * DAY;
  state.trialLease = h.sign(spec);
  state.lastSeen = NOW;
  h.store(state);
  EXPECT_EQ(h.license().status().state, LicenseState::Trial);
}

}  // namespace
}  // namespace kgtest
