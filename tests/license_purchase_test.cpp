// Buying from the app with no key typed: purchaseUrl() and the pickup that status() and
// startTrial() make of the key bought with its token.
#include <future>
#include <regex>
#include <utility>

#include "pickup.hpp"
#include "purchase_support.hpp"
#include "support.hpp"

namespace kgtest {
namespace {

using namespace std::chrono_literals;

// --- purchaseUrl ------------------------------------------------------------------------

TEST(PurchaseUrl, KeepsANewSecretOf256RandomBitsAndPutsOnlyItsReferenceInTheLink) {
  Harness h;
  const std::optional<std::string> url = h.license().purchaseUrl(LINK);
  ASSERT_TRUE(url);
  const std::string secret = h.stored()->purchaseToken.value_or("");
  EXPECT_TRUE(std::regex_match(secret, std::regex("^[A-Za-z0-9_-]{43}$"))) << secret;
  EXPECT_EQ(*url, LINK + "?client_reference_id=" + keygrant::detail::purchaseReference(secret));
  EXPECT_EQ(url->find(secret), std::string::npos);
  EXPECT_EQ(h.stored()->purchaseTokenAt, NOW);
  EXPECT_TRUE(h.http->actions().empty());
}

/** newPurchaseSecret()'s source swapped for a test's, and put back. */
class SourceSwap {
 public:
  explicit SourceSwap(keygrant::detail::RandomSource source)
      : saved_(std::exchange(keygrant::detail::purchaseSecretSource(), source)) {}
  ~SourceSwap() { keygrant::detail::purchaseSecretSource() = saved_; }
  SourceSwap(const SourceSwap&) = delete;
  SourceSwap& operator=(const SourceSwap&) = delete;

 private:
  keygrant::detail::RandomSource saved_;
};

/** How many bytes the test's source was asked for. */
std::size_t drawn = 0;

TEST(PurchaseUrl, DrawsItsSecretFrom32BytesOfTheCsprngAndEncodesThem) {
  // The source in use gives random bytes: two draws of 32 differ. (Its address
  // is not compared with csprngBytes: across a DLL a function's address is not
  // one.)
  unsigned char first[32] = {};
  unsigned char second[32] = {};
  ASSERT_TRUE(keygrant::detail::purchaseSecretSource()(first, sizeof first));
  ASSERT_TRUE(keygrant::detail::purchaseSecretSource()(second, sizeof second));
  EXPECT_NE(std::string(first, first + 32), std::string(second, second + 32));
  // What the source gives is the secret: the bytes 0 to 31, base64url.
  const SourceSwap swap([](unsigned char* out, std::size_t size) {
    drawn = size;
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<unsigned char>(i);
    return true;
  });
  Harness h;
  ASSERT_TRUE(h.license().purchaseUrl(LINK));
  EXPECT_EQ(drawn, 32u);
  EXPECT_EQ(h.stored()->purchaseToken, "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");
}

TEST(PurchaseUrl, ThrowsWhenTheCsprngGivesNothingSavingNothing) {
  const SourceSwap swap([](unsigned char*, std::size_t) { return false; });
  Harness h;
  try {
    h.license().purchaseUrl(LINK);
    ADD_FAILURE() << "no exception";
  } catch (const keygrant::IoError&) {
    ADD_FAILURE() << "the storage's exception, not the CSPRNG's";
  } catch (const std::runtime_error&) {
    // The CSPRNG gave no secret.
  }
  EXPECT_EQ(h.storage->writes(), 0);
  EXPECT_EQ(h.stored(), std::nullopt);
}

TEST(PurchaseUrl, HandsTheHeldTokenOutAgainItsBoundCountedFromThisCallAndClearsTheClaimsWait) {
  StoredState over;
  over.claimAskedAt = NOW - 1000;
  Harness h(tokenHeld(10 * DAY, over));
  h.setClock(NOW + 1000);
  EXPECT_EQ(h.license().purchaseUrl(LINK), LINK + "?client_reference_id=" + REF);
  EXPECT_EQ(h.stored()->purchaseToken, TOKEN);
  EXPECT_EQ(h.stored()->purchaseTokenAt, NOW + 1000);
  EXPECT_FALSE(h.stored()->claimAskedAt);
}

TEST(PurchaseUrl, MakesANewTokenOnceTheHeldOneIsPastItsBound) {
  Harness h(tokenHeld(keygrant::detail::kPurchaseTokenMs));
  const std::optional<std::string> url = h.license().purchaseUrl(LINK);
  ASSERT_TRUE(url);
  EXPECT_EQ(url->find(REF), std::string::npos);
  EXPECT_NE(h.stored()->purchaseToken, TOKEN);
  EXPECT_EQ(h.stored()->purchaseTokenAt, NOW);
}

TEST(PurchaseUrl, KeepsTheLinksOwnParametersAndFragmentAndReplacesAClientReferenceIdAlreadyOnIt) {
  Harness h(tokenHeld());
  EXPECT_EQ(h.license().purchaseUrl("https://buy.stripe.com/x?prefilled_email=jo%40example.com&client_reference_id=old#pay"),
            "https://buy.stripe.com/x?prefilled_email=jo%40example.com&client_reference_id=" + REF + "#pay");
}

TEST(PurchaseUrl, IsNoneWhileAnyKeyIsHeldAndSavesNothing) {
  StoredState leased = held("header.payload.sig", "KEY-1");
  StoredState refused;
  refused.key = "KEY-1";
  refused.refused = keygrant::Refusal::Revoked;
  StoredState keyOnly;
  keyOnly.key = "KEY-1";
  for (const StoredState& state : {leased, refused, keyOnly}) {
    Harness h(state);
    EXPECT_EQ(h.license().purchaseUrl(LINK), std::nullopt);
    EXPECT_EQ(h.storage->writes(), 0);
  }
}

TEST(PurchaseUrl, RefusesALinkThatIsNotAnAbsoluteUrlBeforeReadingAnything) {
  Harness h;
  // A read would throw the storage's IoError: the link is refused first.
  h.storage->failReads = true;
  for (const char* link : {"buy.stripe.com/test_abc", "/buy", ""}) {
    EXPECT_THROW(h.license().purchaseUrl(link), std::invalid_argument) << link;
  }
}

TEST(PurchaseUrl, ThrowsWhenTheStateCannotBeReadOrTheTokenCannotBeSaved) {
  Harness unreadable;
  unreadable.storage->failNextRead = true;
  EXPECT_THROW(unreadable.license().purchaseUrl(LINK), keygrant::IoError);
  Harness unsaved;
  unsaved.storage->failWrites = true;
  EXPECT_THROW(unsaved.license().purchaseUrl(LINK), keygrant::IoError);
}

TEST(PurchaseUrl, BehindACallThatHasNotFinishedHandsOutTheHeldTokenAsItIsAndThrowsWhenNoneIsHeld) {
  Harness h(tokenHeld(DAY), quick());
  auto stuck = stuckStatus(h);
  EXPECT_EQ(h.license().purchaseUrl(LINK), LINK + "?client_reference_id=" + REF);
  EXPECT_EQ(h.storage->writes(), 0);
  h.storage->readGate().open();
  finish(stuck);

  Harness none(std::nullopt, quick());
  auto stuckToo = stuckStatus(none);
  EXPECT_THROW(none.license().purchaseUrl(LINK), keygrant::StalledError);
  EXPECT_EQ(none.storage->writes(), 0);
  none.storage->readGate().open();
  finish(stuckToo);
}

TEST(PurchaseUrl, BehindACallThatHasNotFinishedThrowsForAHeldSecretPastItsBound) {
  // Its link would carry a secret the next status() drops: a purchase never picked up.
  Harness h(tokenHeld(keygrant::detail::kPurchaseTokenMs), quick());
  auto stuck = stuckStatus(h);
  EXPECT_THROW(h.license().purchaseUrl(LINK), keygrant::StalledError);
  EXPECT_EQ(h.storage->writes(), 0);
  h.storage->readGate().open();
  finish(stuck);
}

// --- the pickup: status() asks for the key bought with the token -------------------------

TEST(Pickup, SavesABoughtKeyExactlyAsActivateDoesClearsTheTokenAndAnswersLicensed) {
  HarnessOptions options;
  options.deviceName = "Rae\xE2\x80\x99s laptop";
  Harness h(std::nullopt, options);
  h.store(trialState(h, tokenHeld()));
  h.http->on("claim", [&h](const json&) { return bought(h); });
  EXPECT_EQ(h.license().status(), licensed(BOUGHT_KEY));
  // The secret, never the reference, and every fingerprint this device answers to, as a validate sends them.
  EXPECT_EQ(h.http->bodyOf("claim"), json({{"token", TOKEN},
                                           {"fingerprint", "fp-test"},
                                           {"fingerprints", {"fp-test"}},
                                           {"name", "Rae\xE2\x80\x99s laptop"},
                                           {"major", 1}}));
  const StoredState stored = *h.stored();
  EXPECT_EQ(stored.key, BOUGHT_KEY);
  EXPECT_EQ(stored.activationId, "act_1");
  EXPECT_EQ(stored.checkedMajor, 1);
  EXPECT_EQ(stored.verdictAt, NOW);
  EXPECT_TRUE(noPurchase(stored));
  // Licensed from then on, offline, and never asked again.
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(BOUGHT_KEY));
  EXPECT_EQ(h.http->count("claim"), 1);
  EXPECT_FALSE(h.http->called("trial"));
}

TEST(Pickup, OnNotBoughtYetAnswersTheTrialKeepsTheTokenAndAsksAgainOnlyAfterTheWait) {
  Harness h;
  h.store(trialState(h, tokenHeld()));
  h.http->reply("claim", 200, json{{"pending", true}});
  EXPECT_EQ(h.license().status(), runningTrial());
  EXPECT_EQ(h.stored()->purchaseToken, TOKEN);
  EXPECT_EQ(h.stored()->claimAskedAt, NOW);
  h.setClock(NOW + keygrant::detail::kClaimWaitMs - 1);
  h.license().status();
  EXPECT_EQ(h.http->count("claim"), 1);
  h.setClock(NOW + keygrant::detail::kClaimWaitMs);
  h.license().status();
  EXPECT_EQ(h.http->count("claim"), 2);
}

TEST(Pickup, AsksAtOnceOnRefreshAndWhenTheClockWasSetBackBeforeTheLastAsk) {
  Harness h;
  StoredState over;
  over.claimAskedAt = NOW - 1000;
  h.store(trialState(h, tokenHeld(0, over)));
  h.http->reply("claim", 200, json{{"pending", true}});
  h.license().refresh();
  EXPECT_EQ(h.http->count("claim"), 1);
  StoredState back = *h.stored();
  back.claimAskedAt = NOW + 60'000;
  h.store(back);
  h.license().status();
  EXPECT_EQ(h.http->count("claim"), 2);
}

TEST(Pickup, KeepsTheTokenWhenNothingAnswersOrTheAnswerCarriesNoErrorWord) {
  const std::vector<std::optional<keygrant::HttpResponse>> answers = {
      std::nullopt,
      keygrant::HttpResponse{503, R"({"error":"internal"})"},
      keygrant::HttpResponse{404, "<html>"},
      keygrant::HttpResponse{200, R"("portal")"},
  };
  for (const auto& answer : answers) {
    SCOPED_TRACE(answer ? answer->body : "no answer");
    Harness h(tokenHeld());
    if (answer) {
      h.http->on("claim", [answer](const json&) { return *answer; });
    } else {
      h.http->fail = true;
    }
    EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
    EXPECT_EQ(h.stored()->purchaseToken, TOKEN);
    EXPECT_EQ(h.stored()->claimAskedAt, NOW);
  }
}

TEST(Pickup, DropsTheTokenOnAnErrorWordAndOnAKeyWhoseLeaseDoesNotLicenseThisBuild) {
  Harness h;
  LeaseSpec elsewhere;
  elsewhere.sub = BOUGHT_KEY;
  elsewhere.product = "another-product";
  elsewhere.ttlSeconds = 86'400;
  const std::vector<keygrant::HttpResponse> answers = {
      {409, R"({"error":"spent"})"},
      {403, R"({"error":"pickup-off"})"},
      {403, R"({"error":"limit"})"},
      {200, json({{"key", BOUGHT_KEY}, {"activationId", "act_1"}, {"lease", h.sign(elsewhere)}}).dump()},
  };
  for (const auto& answer : answers) {
    SCOPED_TRACE(answer.body);
    h.store(trialState(h, tokenHeld()));
    h.http->on("claim", [answer](const json&) { return answer; });
    EXPECT_EQ(h.license().status(), runningTrial());
    EXPECT_TRUE(noPurchase(h.stored()));
    EXPECT_FALSE(h.stored()->key);
  }
  h.license().refresh();
  EXPECT_EQ(h.http->count("claim"), static_cast<int>(answers.size()));
}

TEST(Pickup, DropsATokenPastItsBoundWithoutSendingIt) {
  Harness h(tokenHeld(keygrant::detail::kPurchaseTokenMs));
  EXPECT_EQ(h.license().refresh(), status(LicenseState::Unlicensed));
  EXPECT_FALSE(h.http->called("claim"));
  EXPECT_TRUE(noPurchase(h.stored()));
}

TEST(Pickup, SavesADropAboutItsTokenATokenAnotherProcessMadeMeanwhileStays) {
  Harness h(tokenHeld());
  const std::string another = "kgp_another-process-made-this-one-meanwhile";
  h.http->on("claim", [&h, another](const json&) {
    StoredState theirs;
    theirs.purchaseToken = another;
    theirs.purchaseTokenAt = NOW;
    h.store(theirs);
    return keygrant::HttpResponse{409, R"({"error":"spent"})"};
  });
  h.license().status();
  EXPECT_EQ(h.stored()->purchaseToken, another);
  EXPECT_EQ(h.stored()->purchaseTokenAt, NOW);
}

TEST(Pickup, BoundsAnUnforcedClaimAsAStatusCheckIsAndAForcedOneByTheHttpBound) {
  Harness h(tokenHeld(), quick());
  h.http->hang = true;
  LicenseStatus answered;
  const Millis took = timed([&] { answered = h.license().status(); });
  EXPECT_EQ(answered, status(LicenseState::Unlicensed));
  EXPECT_GE(took, h.timings.statusCheck);
  EXPECT_LT(took, 3s);
  // The abandoned request's thread records it when it runs, maybe after its bound.
  ASSERT_TRUE(h.http->waitFor("claim", 1, 5s));
  EXPECT_EQ(h.http->timeouts(), std::vector<Millis>{h.timings.statusCheck});
  EXPECT_EQ(h.stored()->purchaseToken, TOKEN);
  EXPECT_EQ(h.stored()->claimAskedAt, NOW);
  // refresh(): under the HTTP bound.
  h.license().refresh();
  ASSERT_TRUE(h.http->waitFor("claim", 2, 5s));
  EXPECT_EQ(h.http->timeouts(), (std::vector<Millis>{h.timings.statusCheck, Millis{100}}));
}

TEST(PickupAtStartTrial, BoundsItsClaimAsAStatusCheckIsThenAsksForTheTrial) {
  Harness h(tokenHeld(), quick());
  h.http->hang = true;
  EXPECT_EQ(h.license().startTrial(), status(LicenseState::Unlicensed));
  // The abandoned trial ask's thread records it when it runs, maybe after its bound.
  ASSERT_TRUE(h.http->waitFor("trial", 1, 5s));
  EXPECT_EQ(h.http->actions(), (std::vector<std::string>{"claim", "trial"}));
  EXPECT_EQ(h.http->timeouts(), (std::vector<Millis>{h.timings.statusCheck, Millis{100}}));
  EXPECT_EQ(h.stored()->purchaseToken, TOKEN);
  EXPECT_EQ(h.stored()->claimAskedAt, NOW);
}

TEST(PickupAtStartTrial, ReadsTheDeviceOnceForBothAsksAndMayClaimUnderTheHostName) {
  auto host = std::make_shared<HostOnly>();
  HarnessOptions options;
  options.fingerprint = host;
  Harness h(tokenHeld(), options);
  h.http->reply("claim", 200, json{{"pending", true}});
  EXPECT_EQ(h.license().startTrial(), status(LicenseState::Unlicensed));
  EXPECT_EQ(host->calls.load(), 1);
  EXPECT_EQ(h.http->bodyOf("claim"),
            json({{"token", TOKEN},
                  {"fingerprint", "fp-host"},
                  {"fingerprintKind", "hostname"},
                  {"fingerprints", {"fp-host"}},
                  {"major", 1}}));
}

TEST(Pickup, AnswersABoughtKeyEvenWhenTheDiskRefusesItAndTheNextStatusAsksAgainWithTheSameFingerprints) {
  Harness h(tokenHeld());
  h.http->on("claim", [&h](const json&) { return bought(h); });
  h.storage->failWrites = true;
  EXPECT_EQ(h.license().status(), licensed(BOUGHT_KEY));
  h.storage->failWrites = false;
  EXPECT_EQ(h.license().status(), licensed(BOUGHT_KEY));
  const std::vector<json> bodies = h.http->bodiesOf("claim");
  ASSERT_EQ(bodies.size(), 2u);
  EXPECT_EQ(bodies[1], bodies[0]);
  EXPECT_EQ(bodies[0]["fingerprints"], json({"fp-test"}));
  EXPECT_EQ(h.stored()->key, BOUGHT_KEY);
}

TEST(Pickup, SendsEveryFingerprintTheRunReadWhenTheDeviceIsNotHeldToo) {
  auto own = std::make_shared<FakeFingerprinter>("fp-own");
  own->identifyAs(DeviceKind::Machine, {"fp-host"});
  HarnessOptions options;
  options.fingerprint = own;
  options.deviceHold = false;
  Harness h(tokenHeld(), options);
  h.http->reply("claim", 200, json{{"pending", true}});
  h.license().status();
  // Its own and the host name, and no kind: a device not held is keyed by nothing.
  EXPECT_EQ(h.http->bodyOf("claim"),
            json({{"token", TOKEN}, {"fingerprint", "fp-own"}, {"fingerprints", {"fp-own", "fp-host"}}, {"major", 1}}));
}

TEST(Pickup, SendsAtMostFourFingerprintsOwnFirstEachOfOneTo200Characters) {
  auto many = std::make_shared<FakeFingerprinter>("fp-own");
  many->identifyAs(DeviceKind::Machine, {"fp-a1", std::string(201, 'x'), "fp-a2", "", std::string(200, 'y'), "fp-a4"});
  HarnessOptions options;
  options.fingerprint = many;
  Harness h(tokenHeld(), options);
  h.http->reply("claim", 200, json{{"pending", true}});
  h.license().status();
  const json body = h.http->bodyOf("claim");
  EXPECT_EQ(body["fingerprint"], "fp-own");
  EXPECT_EQ(body["fingerprints"], json({"fp-own", "fp-a1", "fp-a2", std::string(200, 'y')}));
}

TEST(Pickup, NeverClaimsUnderAHostNameFallbackFromStatusOrRefreshNoAskAndNoWaitStarted) {
  HarnessOptions options;
  options.fingerprint = std::make_shared<HostOnly>();
  Harness h(tokenHeld(), options);
  h.http->reply("claim", 200, json{{"pending", true}});
  EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
  EXPECT_EQ(h.license().refresh(), status(LicenseState::Unlicensed));
  EXPECT_FALSE(h.http->called("claim"));
  EXPECT_FALSE(h.stored()->claimAskedAt);
  EXPECT_EQ(h.stored()->purchaseToken, TOKEN);
}

TEST(Pickup, ReadsTheDevice2SForAStatussClaimAndOnceFor10SAtATrialStart) {
  // The SDK's own read bounds (Timings{}): a status() waits no longer than a
  // status's read on a machine id that does not read.
  auto timed = std::make_shared<TimedId>();
  HarnessOptions options;
  options.fingerprint = timed;
  Harness h(tokenHeld(), options);
  h.http->reply("claim", 200, json{{"pending", true}});
  EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
  EXPECT_EQ(timed->timeouts(), std::vector<Millis>{h.timings.machineId});
  EXPECT_FALSE(h.http->called("claim"));
  EXPECT_FALSE(h.stored()->claimAskedAt);

  auto atStart = std::make_shared<TimedId>();
  HarnessOptions startOptions;
  startOptions.fingerprint = atStart;
  Harness start(tokenHeld(), startOptions);
  start.http->reply("claim", 200, json{{"pending", true}});
  start.license().startTrial();
  EXPECT_EQ(atStart->timeouts(), std::vector<Millis>{start.timings.activateMachineId});
  EXPECT_EQ(start.http->bodyOf("claim")["fingerprint"], "fp-host");
  EXPECT_FALSE(start.http->called("trial"));
}

TEST(Pickup, AsksUnforcedOnlyForADayAfterTheLinkThenOnlyRefreshAsks) {
  Harness fresh(tokenHeld(keygrant::detail::kClaimEagerMs - 1));
  fresh.http->reply("claim", 200, json{{"pending", true}});
  fresh.license().status();
  EXPECT_EQ(fresh.http->count("claim"), 1);

  Harness stale(tokenHeld(keygrant::detail::kClaimEagerMs));
  stale.http->reply("claim", 200, json{{"pending", true}});
  EXPECT_EQ(stale.license().status(), status(LicenseState::Unlicensed));
  EXPECT_FALSE(stale.http->called("claim"));
  stale.license().refresh();
  EXPECT_EQ(stale.http->count("claim"), 1);
}

TEST(Pickup, ALinkHandedOutAgainWhileAClaimIsOutKeepsItsClearedWait) {
  Harness h(tokenHeld(DAY / 2));
  h.http->on("claim", [&h](const json&) {
    // Another process's purchaseUrl() hands the same secret's link out again, clearing the wait.
    StoredState again = *h.stored();
    again.purchaseTokenAt = NOW;
    again.claimAskedAt.reset();
    h.store(again);
    return keygrant::HttpResponse{200, R"({"pending":true})"};
  });
  h.license().status();
  EXPECT_EQ(h.stored()->purchaseToken, TOKEN);
  EXPECT_EQ(h.stored()->purchaseTokenAt, NOW);
  EXPECT_FALSE(h.stored()->claimAskedAt);
}

TEST(Pickup, AsksNothingWhileAKeyIsHeldOrBehindACallThatHasNotFinished) {
  StoredState keyedState = tokenHeld();
  keyedState.key = "KEY-1";
  Harness keyed(keyedState);
  keyed.license().refresh();
  EXPECT_FALSE(keyed.http->called("claim"));

  Harness h(tokenHeld(), quick());
  auto stuck = stuckStatus(h);
  EXPECT_EQ(h.license().status(true), stalled(status(LicenseState::Unlicensed)));
  EXPECT_FALSE(h.http->called("claim"));
  h.storage->readGate().open();
  finish(stuck);
}

TEST(Pickup, KeepsTheTokenOnAnAnswerNestedDeepNeverACrash) {
  for (const std::size_t depth : kDeepNesting) {
    SCOPED_TRACE(depth);
    const std::string deep = nestedArrays(depth);
    Harness h(tokenHeld());
    h.http->on("claim", [deep](const json&) { return keygrant::HttpResponse{200, deep}; });
    EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
    EXPECT_EQ(h.stored()->purchaseToken, TOKEN);
    EXPECT_EQ(h.stored()->claimAskedAt, NOW);
  }
}

// --- the pickup at startTrial() ------------------------------------------------------------

TEST(PickupAtStartTrial, AsksTheClaimBeforeTheTrialWhateverTheWaitAndABoughtKeyIsTheAnswer) {
  StoredState over;
  over.claimAskedAt = NOW - 1000;
  Harness h(tokenHeld(0, over));
  h.http->on("claim", [&h](const json&) { return bought(h); });
  EXPECT_EQ(h.license().startTrial(), licensed(BOUGHT_KEY));
  EXPECT_EQ(h.http->actions(), std::vector<std::string>{"claim"});
}

TEST(PickupAtStartTrial, AsksForTheTrialAfterAClaimThatBroughtNoKey) {
  Harness h(tokenHeld());
  const std::string lease = h.sign("trial:fp-test", "trial", 5 * 24 * 3600);
  h.http->reply("claim", 200, json{{"pending", true}});
  h.http->reply("trial", 200, json{{"trialStart", NOW}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", lease}});
  EXPECT_EQ(h.license().startTrial(), runningTrial());
  EXPECT_EQ(h.http->actions(), (std::vector<std::string>{"claim", "trial"}));
  EXPECT_EQ(h.stored()->purchaseToken, TOKEN);
  EXPECT_EQ(h.stored()->claimAskedAt, NOW);
}

// --- an activation and a deactivation clear the purchase secret ------------------------

TEST(PurchaseToken, IsClearedByATypedKeySoALaterDeactivateLeavesNoPickupPending) {
  Harness h(tokenHeld());
  h.http->reply("activate", 200, json{{"lease", h.sign("KEY-1", "perpetual", 30 * 24 * 3600)}, {"activationId", "act_1"}});
  EXPECT_EQ(h.license().activate("KEY-1"), ok());
  EXPECT_TRUE(noPurchase(h.stored()));
}

TEST(PurchaseToken, IsClearedByDeactivate) {
  StoredState state = tokenHeld();
  state.key = "KEY-1";
  state.activationId = "act_1";
  Harness h(state);
  h.http->reply("deactivate", 200, json::object());
  h.license().deactivate();
  EXPECT_TRUE(noPurchase(h.stored()));
}

// --- a key bought from a trial model's own price (model trial) --------------------------

TEST(TrialModelKey, LicensesTheAppAsALicenceWhenPickedUpAndIsNeverReadAsATrial) {
  Harness h(tokenHeld());
  h.http->on("claim", [&h](const json&) { return bought(h, "trial"); });
  EXPECT_EQ(h.license().status(), licensed(BOUGHT_KEY));
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(BOUGHT_KEY));
  EXPECT_FALSE(h.stored()->trialLease);
}

TEST(TrialModelKey, LicensesATypedKeyAsWellOffline) {
  Harness h;
  h.store(held(h.sign("KEY-1", "trial", 30 * 24 * 3600), "KEY-1"));
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed("KEY-1"));
}

// --- every answer parser ignores members it does not know -----------

TEST(Answers, AreReadIgnoringEveryMemberAParserDoesNotKnow) {
  const json extra = {{"future", {{"nested", {1, 2}}}}, {"note", "added by a later server"}};
  const auto with = [&extra](json body) {
    body.update(extra);
    return body;
  };
  Harness h(tokenHeld());
  const std::string lease = h.sign("KEY-1", "perpetual", 30 * 24 * 3600);
  h.http->reply("claim", 200, with({{"key", "KEY-1"}, {"activationId", "act_1"}, {"lease", lease}}));
  h.http->reply("validate", 200, with({{"lease", lease}}));
  h.http->reply("activate", 200, with({{"lease", lease}, {"activationId", "act_1"}}));
  EXPECT_EQ(h.license().status(), licensed("KEY-1"));
  EXPECT_EQ(h.license().refresh(), licensed("KEY-1"));
  EXPECT_EQ(h.license().activate("KEY-1"), ok());
  EXPECT_EQ(h.http->actions(), (std::vector<std::string>{"claim", "validate", "activate"}));

  Harness trial;
  const std::string trialLease = trial.sign("trial:fp-test", "trial", 5 * 24 * 3600);
  trial.http->reply("trial", 200, with({{"trialStart", NOW}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", trialLease}}));
  EXPECT_EQ(trial.license().startTrial(), runningTrial());
}

}  // namespace
}  // namespace kgtest
