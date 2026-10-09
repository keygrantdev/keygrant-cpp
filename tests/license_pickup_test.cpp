// The pickup's edges: a bought key's lease verified before it is saved, each stamp and check on its
// own clock, the claim saves other than a bought key's, the secret in no request but the claim, and
// the extra grace behind startTrial().
#include <future>
#include <thread>

#include "crypto.hpp"
#include "lease.hpp"
#include "pickup.hpp"
#include "purchase_support.hpp"
#include "support.hpp"

namespace kgtest {
namespace {

using keygrant::detail::base64UrlDecode;
using keygrant::detail::base64UrlEncode;

/** A claim's 200 carrying a key, an activation and `lease`. */
keygrant::HttpResponse claimedWith(const std::string& lease) {
  return {200, json({{"key", BOUGHT_KEY}, {"activationId", "act_1"}, {"lease", lease}}).dump()};
}

/** A bought key's lease signed by a key that is not the product's. */
std::string otherKeysLease() {
  LeaseSpec spec;
  spec.sub = BOUGHT_KEY;
  return TestKey().sign(claimsOf(spec));
}

/**
 * A bought key's lease signed by the product's key, with one claim changed
 * inside its payload (`lim` 3 to 9) and re-encoded under the original
 * signature: still JSON, still base64url, refused by the signature alone.
 */
std::string tamperedLease(const Harness& h) {
  const std::string lease = h.sign(BOUGHT_KEY, "perpetual", 30 * 24 * 3600);
  const std::size_t first = lease.find('.');
  const std::size_t second = lease.find('.', first + 1);
  const auto payload = base64UrlDecode(lease.substr(first + 1, second - first - 1));
  std::string text(payload->begin(), payload->end());
  const std::size_t at = text.find("\"lim\":3");
  EXPECT_NE(at, std::string::npos) << text;
  text.replace(at, 7, "\"lim\":9");
  const std::string tampered = lease.substr(0, first + 1) + base64UrlEncode(text) + lease.substr(second);
  const auto key = keygrant::detail::VerifyingKey::fromJwk(h.key.publicJwk());
  EXPECT_EQ(keygrant::detail::verifyLease(tampered, key, NOW).reason, keygrant::detail::VerifyFailure::BadSignature);
  return tampered;
}

// --- a bought key's lease is verified before anything is saved ---------------

TEST(PickupVerification, RefusesAClaimWhoseLeaseAnotherKeySignedOrWhosePayloadWasTampered) {
  for (const bool tamper : {false, true}) {
    SCOPED_TRACE(tamper ? "tampered" : "another key");
    Harness h;
    h.store(trialState(h, tokenHeld()));
    const std::string lease = tamper ? tamperedLease(h) : otherKeysLease();
    h.http->on("claim", [lease](const json&) { return claimedWith(lease); });
    // As with no secret: the trial, the secret dropped, no key saved.
    EXPECT_EQ(h.license().status(), runningTrial());
    EXPECT_TRUE(noPurchase(h.stored()));
    EXPECT_FALSE(h.stored()->key);
    EXPECT_FALSE(h.stored()->lease);

    Harness start(tokenHeld());
    const std::string startLease = tamper ? tamperedLease(start) : otherKeysLease();
    start.http->on("claim", [startLease](const json&) { return claimedWith(startLease); });
    start.http->reply("trial", 200,
                      json{{"trialStart", NOW}, {"trialEndsAt", NOW + 5 * DAY},
                           {"lease", start.sign("trial:fp-test", "trial", 5 * 24 * 3600)}});
    EXPECT_EQ(start.license().startTrial(), runningTrial());
    EXPECT_EQ(start.http->actions(), (std::vector<std::string>{"claim", "trial"}));
    EXPECT_TRUE(noPurchase(start.stored()));
    EXPECT_FALSE(start.stored()->key);
  }
}

// --- each stamp and check on its own clock ------------------------------------

/** The guarded clock a day ahead of the device clock: a state that last saw NOW + DAY, the clock at NOW. */
StoredState seenADayAhead(StoredState state) {
  state.lastSeen = NOW + DAY;
  return state;
}

constexpr std::int64_t HOUR = 3'600'000;

TEST(PickupClocks, JudgeTheLinksDayByTheGuardedClockAndTheWaitByTheDeviceClock) {
  // A link an hour old by the device clock is stale by the guarded one.
  Harness stale(seenADayAhead(tokenHeld(HOUR)));
  stale.http->reply("claim", 200, json{{"pending", true}});
  stale.license().status();
  EXPECT_FALSE(stale.http->called("claim"));
  // Fresh by the guarded clock, but asked a second ago by the device clock: the wait still runs.
  StoredState asked;
  asked.claimAskedAt = NOW - 1000;
  Harness waiting(seenADayAhead(tokenHeld(-(DAY - HOUR), asked)));
  waiting.license().status();
  EXPECT_FALSE(waiting.http->called("claim"));
  // Neither was dropped: refresh() asks.
  stale.license().refresh();
  EXPECT_EQ(stale.http->count("claim"), 1);
}

TEST(PickupClocks, StampTheLinkByTheGuardedClockAndTheWaitByTheDeviceClock) {
  Harness link(seenADayAhead(StoredState{}));
  link.license().purchaseUrl(LINK);
  EXPECT_EQ(link.stored()->purchaseTokenAt, NOW + DAY);

  Harness wait(seenADayAhead(tokenHeld(-DAY)));
  wait.http->reply("claim", 200, json{{"pending", true}});
  wait.license().status();
  EXPECT_EQ(wait.http->count("claim"), 1);
  EXPECT_EQ(wait.stored()->claimAskedAt, NOW);
}

TEST(PickupClocks, PurchaseUrlKeepsASecretOnlyWithinItsBoundByTheGuardedClock) {
  // 29 days by the device clock, 30 by the guarded one: past its bound, so a new secret.
  Harness h(seenADayAhead(tokenHeld(keygrant::detail::kPurchaseTokenMs - DAY)));
  ASSERT_TRUE(h.license().purchaseUrl(LINK));
  EXPECT_NE(h.stored()->purchaseToken, TOKEN);
  EXPECT_EQ(h.stored()->purchaseTokenAt, NOW + DAY);
  // And behind a call that has not finished, none is held to hand out.
  HarnessOptions options = quick();
  Harness stuck(seenADayAhead(tokenHeld(keygrant::detail::kPurchaseTokenMs - DAY)), options);
  auto pending = stuckStatus(stuck);
  EXPECT_THROW(stuck.license().purchaseUrl(LINK), keygrant::StalledError);
  EXPECT_EQ(stuck.storage->writes(), 0);
  stuck.storage->readGate().open();
  finish(pending);
}

TEST(PickupClocks, DropASecretPastItsBoundByTheGuardedClock) {
  // 29 days by the device clock, 30 by the guarded one.
  Harness h(seenADayAhead(tokenHeld(keygrant::detail::kPurchaseTokenMs - DAY)));
  h.license().refresh();
  EXPECT_FALSE(h.http->called("claim"));
  EXPECT_TRUE(noPurchase(h.stored()));
}

// --- the claim saves other than a bought key's -------------------------------

TEST(PickupSaves, DropASecretPastItsBoundAboutItSoAnotherProcesssNewSecretStays) {
  Harness h(tokenHeld(keygrant::detail::kPurchaseTokenMs));
  // Another process hands out a new secret between this status()'s read and its drop's save.
  StoredState theirs;
  theirs.purchaseToken = "another-process-made-this-secret-meanwhile";
  theirs.purchaseTokenAt = NOW;
  h.storage->storeBeforeRead(2, theirs);
  EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
  EXPECT_EQ(h.stored()->purchaseToken, theirs.purchaseToken);
  EXPECT_EQ(h.stored()->purchaseTokenAt, NOW);
}

TEST(PickupSaves, DropASpentSecretEvenAfterItsLinkWasHandedOutAgain) {
  Harness h(tokenHeld(DAY / 2));
  h.http->on("claim", [&h](const json&) {
    // Another process's purchaseUrl() hands the same secret's link out again: a drop carries no `at`.
    h.store(tokenHeld(-1000));
    return keygrant::HttpResponse{409, R"({"error":"spent"})"};
  });
  h.license().status();
  EXPECT_TRUE(noPurchase(h.stored()));
}

TEST(PickupSaves, AnswerAllTheSameWhenTheDiskRefusesAWaitOrADrop) {
  Harness wait(tokenHeld());
  wait.http->fail = true;
  wait.storage->failWrites = true;
  EXPECT_EQ(wait.license().status(), status(LicenseState::Unlicensed));

  Harness drop(tokenHeld());
  drop.http->reply("claim", 409, json{{"error", "spent"}});
  drop.storage->failWrites = true;
  EXPECT_EQ(drop.license().status(), status(LicenseState::Unlicensed));
  EXPECT_EQ(drop.http->count("claim"), 1);

  // A secret past its bound, dropped unsent, the drop refused by the disk.
  Harness past(tokenHeld(keygrant::detail::kPurchaseTokenMs));
  past.storage->failWrites = true;
  EXPECT_EQ(past.license().status(), status(LicenseState::Unlicensed));
  EXPECT_FALSE(past.http->called("claim"));
}

// --- a bought key: judged at the server's time, keyed by its machine id -----

TEST(PickupBought, IsJudgedAtTheServersTimeTheClockGuardBroughtBack) {
  // A clock that once ran 60 days ahead: the bought lease (signed now, 30 days) is not expired.
  StoredState state = tokenHeld(-60 * DAY);
  state.lastSeen = NOW + 60 * DAY;
  Harness h(state);
  h.http->on("claim", [&h](const json&) { return bought(h); });
  EXPECT_EQ(h.license().status(), licensed(BOUGHT_KEY));
  EXPECT_EQ(h.stored()->key, BOUGHT_KEY);
  EXPECT_EQ(h.stored()->lastSeen, NOW);
}

TEST(PickupBought, UnderAMachineIdKeysTheInstallByIt) {
  auto machine = std::make_shared<FakeFingerprinter>("fp-machine");
  machine->identifyAs(DeviceKind::Machine, {});
  HarnessOptions options;
  options.fingerprint = machine;
  Harness h(tokenHeld(), options);
  h.http->on("claim", [&h](const json&) { return bought(h); });
  EXPECT_EQ(h.license().status(), licensed(BOUGHT_KEY));
  EXPECT_EQ(h.stored()->deviceKind, DeviceKind::Machine);
}

// --- the secret goes in no request but the claim -----------------------------

TEST(PickupSecret, IsSentInNoRequestButTheClaim) {
  Harness h;
  StoredState state = tokenHeld();
  state.trialStart = NOW - 2 * DAY;
  state.trialEndsAt = NOW + 5 * DAY;
  const std::string trialLease = h.sign("trial:fp-test", "trial", 5 * 24 * 3600);
  state.trialLease = trialLease;
  state.lastSeen = NOW;
  h.store(state);
  const std::string lease = h.sign("KEY-1", "perpetual", 30 * 24 * 3600);
  h.http->reply("claim", 200, json{{"pending", true}});
  h.http->reply("trial", 200, json{{"trialStart", NOW - 2 * DAY}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", trialLease}});
  h.http->reply("activate", 200, json{{"lease", lease}, {"activationId", "act_1"}});
  h.http->reply("validate", 200, json{{"lease", lease}});
  h.license().status();  // a claim and the launch report
  ASSERT_TRUE(h.reportSettled());
  h.license().startTrial();  // a claim and the trial ask
  h.license().activate("KEY-1");
  h.license().refresh();
  const std::vector<std::string> actions = h.http->actions();
  EXPECT_EQ(actions, (std::vector<std::string>{"claim", "trial", "claim", "trial", "activate", "validate"}));
  for (const std::string action : {"claim", "trial", "activate"}) {
    for (const json& body : h.http->bodiesOf(action)) {
      EXPECT_EQ(body.dump().find(TOKEN) != std::string::npos, action == "claim") << action << ": " << body.dump();
    }
  }

  // A key and a secret stored together (activate() clears the secret, so the
  // validate above had none to leak): neither a validate nor a deactivate
  // carries it.
  Harness keyed;
  StoredState both = tokenHeld();
  const StoredState key = held(keyed.sign("KEY-1", "perpetual", 30 * 24 * 3600), "KEY-1");
  both.key = key.key;
  both.activationId = key.activationId;
  both.lease = key.lease;
  both.checkedMajor = key.checkedMajor;
  both.lastSeen = key.lastSeen;
  keyed.store(both);
  keyed.http->reply("validate", 200, json{{"lease", *key.lease}});
  keyed.http->reply("deactivate", 200, json::object());
  keyed.license().refresh();
  keyed.license().deactivate();
  EXPECT_EQ(keyed.http->actions(), (std::vector<std::string>{"validate", "deactivate"}));
  for (const std::string action : {"validate", "deactivate"}) {
    const json body = keyed.http->bodyOf(action);
    EXPECT_EQ(body.dump().find(TOKEN), std::string::npos) << action << ": " << body.dump();
  }
}

// --- startTrial(): no fingerprint at all, and behind a stuck call ------------

TEST(PickupAtStartTrial, WithNoFingerprintAtAllSendsNoClaimStartsTheWaitAndStillGoesOnToTheTrial) {
  auto none = std::make_shared<FakeFingerprinter>();
  none->failing = true;
  HarnessOptions options;
  options.fingerprint = none;
  Harness h(tokenHeld(), options);
  EXPECT_EQ(h.license().startTrial(), status(LicenseState::Unlicensed));
  EXPECT_TRUE(h.http->actions().empty());
  EXPECT_EQ(h.stored()->claimAskedAt, NOW);
  EXPECT_EQ(h.stored()->purchaseToken, TOKEN);
  // The trial ask was made (its stash read), and sent nothing for want of a fingerprint.
  EXPECT_EQ(h.registry->reads.load(), 1);
}

TEST(PickupAtStartTrial, BehindACallThatHasNotFinishedClaimsNothing) {
  Harness h(tokenHeld(), quick());
  auto stuck = stuckStatus(h);
  EXPECT_EQ(h.license().startTrial(), stalled(status(LicenseState::Unlicensed)));
  EXPECT_FALSE(h.http->called("claim"));
  EXPECT_EQ(h.storage->writes(), 0);
  h.storage->readGate().open();
  finish(stuck);
}

// --- the extra grace behind startTrial(): from below and from above -----------

/** A machine id that reads only after `delay`. */
class SlowMachine final : public keygrant::Fingerprinter {
 public:
  explicit SlowMachine(Millis delay) : delay_(delay) {}
  std::string get() override { return "unused"; }
  std::optional<keygrant::DeviceIdentity> identify(Millis) override {
    std::this_thread::sleep_for(delay_);
    keygrant::DeviceIdentity identity;
    identity.fingerprint = "fp-slow";
    identity.kind = DeviceKind::Machine;
    return identity;
  }

 private:
  Millis delay_;
};

/**
 * The SDK's proportions with every bound distinct, so that none stands in for
 * the extra: HTTP 800 ms, lock grace 1300 ms, an activation's machine id
 * 1000 ms, and a status check (the extra) 3000 ms, the longest.
 */
HarnessOptions distinctBounds() {
  HarnessOptions options;
  options.httpTimeout = Millis{800};
  options.timings.lockGrace = Millis{1300};
  options.timings.activateMachineId = Millis{1000};
  options.timings.statusCheck = Millis{3000};
  return options;
}

TEST(PickupAtStartTrial, HoldsTheCallsBehindItForItsLongestRunNotRunningThemReadOnly) {
  // A device read of 600 ms, then a claim (3000 ms) and a trial ask (800 ms)
  // that never answer: a 4400 ms run. The call behind goes read-only at
  // 800 + 1300 = 2100 ms without the extra, and at 5100 ms with it (700 ms
  // after the run); an extra of the HTTP bound, the lock grace or the machine
  // id's alone leaves it read-only before the run ends.
  HarnessOptions options = distinctBounds();
  options.fingerprint = std::make_shared<SlowMachine>(Millis{600});
  Harness h(tokenHeld(), options);
  h.http->hang = true;
  auto started = std::async(std::launch::async, [&h] { return h.license().startTrial(); });
  ASSERT_TRUE(h.http->waitFor("claim", 1, 5s));
  const LicenseStatus behind = h.license().status();
  EXPECT_EQ(started.get(), status(LicenseState::Unlicensed));
  // Run after the startTrial() settled, not read-only beside it.
  EXPECT_EQ(behind, status(LicenseState::Unlicensed));
  ASSERT_TRUE(h.http->waitFor("trial", 1, 5s));
  EXPECT_EQ(h.http->actions(), (std::vector<std::string>{"claim", "trial"}));
}

TEST(PickupAtStartTrial, HoldsTheCallsBehindItNoLongerThanTheHttpBoundTheGraceAndAStatusCheck) {
  // A startTrial() stuck on its read of the state: the status() behind it runs
  // read-only once 800 + 1300 + 3000 = 5100 ms have passed since it started, and
  // within 750 ms of that: an extra of the status check plus the HTTP bound
  // (5900 ms), plus the machine id's (6100), plus the grace (6400), or twice the
  // check (8100) would be later.
  Harness h(tokenHeld(), distinctBounds());
  h.storage->hangNextRead();
  const auto begun = std::chrono::steady_clock::now();
  auto stuck = std::async(std::launch::async, [&h] { return h.license().startTrial(); });
  ASSERT_TRUE(h.storage->readGate().waitReached(5s));
  const LicenseStatus behind = h.license().status();
  const auto waited = std::chrono::duration_cast<Millis>(std::chrono::steady_clock::now() - begun);
  EXPECT_EQ(behind, stalled(status(LicenseState::Unlicensed)));
  EXPECT_GE(waited, Millis{5100});
  EXPECT_LT(waited, Millis{5850});
  h.storage->readGate().open();
  // The stuck startTrial() itself, once its read is let go: no trial started (nothing answers one here).
  EXPECT_EQ(stuck.get(), status(LicenseState::Unlicensed));
}

// --- the reference of an ill-formed secret -----------------------------------

TEST(PurchaseReference, HashesAnIllFormedSecretAsJavaScriptHoldsItEachBadByteAsUFFFD) {
  const keygrant::detail::Bytes digest = keygrant::detail::sha256("\xEF\xBF\xBD");
  EXPECT_EQ(keygrant::detail::purchaseReference("\xff"), "kgp_" + base64UrlEncode(digest.data(), digest.size()));
}

}  // namespace
}  // namespace kgtest
