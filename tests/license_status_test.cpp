// status() and refresh(): offline-first, healed online, and failing in favour
// of the customer on anything but an explicit verdict (sdk/src/license.test.ts).
#include "support.hpp"

namespace kgtest {
namespace {

using keygrant::Refusal;

const std::int64_t MONTH_S = 30 * 24 * 3600;

json leaseAnswer(const std::string& lease) { return {{"lease", lease}, {"activationId", "act_1"}}; }

/** A harness holding a lease that ran out two days ago. */
Harness& expiredLease(Harness& h, const std::string& key = KEY) {
  h.store(held(h.sign(key, "perpetual", 3600, NOW - 2 * DAY), key, std::nullopt));
  return h;
}

TEST(Status, IsUnlicensedWithNoState) {
  Harness h;
  EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
}

TEST(Status, IsLicensedFromAValidLeaseWithoutContactingTheServer) {
  Harness h;
  h.store(held(h.sign("KEY", "perpetual", MONTH_S), "KEY", std::nullopt));
  EXPECT_EQ(h.license().status(), licensed("KEY"));
  EXPECT_TRUE(h.http->actions().empty());
}

TEST(Status, RenewsOnlineWhenTheLeaseHasExpired) {
  Harness h;
  expiredLease(h);
  const std::string fresh = h.sign(KEY, "perpetual", MONTH_S);
  h.http->reply("validate", 200, leaseAnswer(fresh));
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.stored()->lease, fresh);
  EXPECT_EQ(h.http->bodyOf("validate"), json({{"key", KEY}, {"activationId", "act_1"}, {"major", 1}, {"fingerprints", {"fp-test"}}}));
}

TEST(Status, StaysExpiredOfflineWhenAnExpiredLeaseCannotBeRenewed) {
  Harness h;
  expiredLease(h);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
}

TEST(Status, LocksOutOnlyOnADefinitiveVerdict) {
  const std::vector<std::pair<const char*, LicenseReason>> verdicts = {
      {"revoked", LicenseReason::Revoked},        {"expired", LicenseReason::Revoked},
      {"deactivated", LicenseReason::Revoked},    {"upgrade-required", LicenseReason::Upgrade},
      {"update-required", LicenseReason::Outdated},
  };
  for (const auto& [word, reason] : verdicts) {
    Harness h;
    expiredLease(h);
    h.http->reply("validate", 403, {{"error", word}});
    EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, reason, KEY)) << word;
  }
}

TEST(Status, FallsBackToExpiredOfflineOnAnAnswerItCannotUse) {
  const std::vector<std::pair<int, json>> answers = {
      {200, {{"lease", "garbage.lease.value"}}},  // unverifiable
      {200, json::object()},                       // no lease
      {200, {{"lease", 42}}},                      // not a string
      {503, {{"error", "unavailable"}}},           // an outage, not a revocation
      {429, {{"error", "rate-limited"}}},
      {404, {{"error", "unknown-product"}}},       // a proxy's or an unknown product's 4xx
      {400, {{"error", "bad"}}},
      {403, {{"error", "limit"}}},                 // not a verdict on validate
  };
  for (const auto& [code, body] : answers) {
    Harness h;
    expiredLease(h);
    h.http->reply("validate", code, body);
    EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Offline, KEY)) << code << body;
  }
  Harness html;
  expiredLease(html);
  html.http->on("validate", [](const json&) { return keygrant::HttpResponse{200, "<html>captive portal</html>"}; });
  EXPECT_EQ(html.license().status(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
}

TEST(Status, KeepsARenewedLeaseThatDoesNotCoverThisBuildAndSaysWhy) {
  HarnessOptions two;
  two.major = 2;
  Harness h(std::nullopt, two);
  expiredLease(h, "KEY");
  const std::string major1 = h.sign("KEY", "perpetual", MONTH_S);
  h.http->reply("validate", 200, leaseAnswer(major1));
  EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, LicenseReason::Upgrade, "KEY"));
  EXPECT_EQ(h.stored()->lease, major1);
}

TEST(Status, IsTamperedForALeaseNotSignedByTheProductsKeyAndHealsOnline) {
  Harness h;
  const TestKey other;
  h.store(held(other.sign(claimsOf(LeaseSpec{})), KEY, std::nullopt));
  h.http->fail = true;
  // Not this licence at all: no key shown.
  EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, LicenseReason::Tampered));
  h.http->fail = false;
  const std::string fresh = h.sign(KEY, "perpetual", MONTH_S);
  h.http->reply("validate", 200, leaseAnswer(fresh));
  EXPECT_EQ(h.license().refresh(), licensed(KEY));
  EXPECT_EQ(h.stored()->lease, fresh);
}

TEST(Status, RefusesAValidlySignedLeaseForAnotherProduct) {
  Harness h;
  LeaseSpec spec;
  spec.sub = "KEY";
  spec.product = "other-app";
  h.store(held(h.sign(spec), "KEY", std::nullopt));
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, LicenseReason::Tampered));
}

TEST(Status, IgnoresAClockPutBackItCannotResurrectAnExpiredLease) {
  Harness h;
  StoredState state = held(h.sign("KEY", "perpetual", 3600, NOW - 2 * DAY), "KEY", std::nullopt);
  state.lastSeen = NOW + 10 * DAY;
  h.store(state);
  h.setClock(NOW - 5 * DAY);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Offline, "KEY"));
}

TEST(Refresh, RenewsAStillValidLeaseAndKeepsItLicensed) {
  Harness h;
  h.store(held(h.sign("KEY", "perpetual", MONTH_S), "KEY", std::nullopt));
  const std::string fresh = h.sign("KEY", "perpetual", MONTH_S, NOW + 1000);
  h.http->reply("validate", 200, leaseAnswer(fresh));
  EXPECT_EQ(h.license().refresh(), licensed("KEY"));
  EXPECT_EQ(h.stored()->lease, fresh);
}

TEST(Refresh, KeepsAValidLeaseLicensedOnAnythingButAVerdict) {
  for (const int code : {0, 500, 429, 404}) {
    Harness h;
    h.store(held(h.sign("KEY", "perpetual", MONTH_S), "KEY", std::nullopt));
    if (code == 0) {
      h.http->fail = true;
    } else {
      h.http->reply("validate", code, {{"error", "whatever"}});
    }
    EXPECT_EQ(h.license().refresh(), licensed("KEY")) << code;
  }
}

std::string rangeLease(const Harness& h, std::int64_t major, std::optional<std::int64_t> minMajor = std::nullopt) {
  LeaseSpec spec;
  spec.sub = "KEY";
  spec.major = major;
  if (minMajor) spec.extra = {{"minMajor", *minMajor}};
  return h.sign(spec);
}

TEST(VersionRanges, AreJudgedOfflineAndRefusedOnlineByTheirOwnWord) {
  HarnessOptions one;
  Harness older(std::nullopt, one);
  older.store(held(rangeLease(older, 3, 2), "KEY", std::nullopt));
  older.http->fail = true;
  EXPECT_EQ(older.license().status(), status(LicenseState::Invalid, LicenseReason::Outdated, "KEY"));
  for (const std::int64_t major : {2, 3}) {
    HarnessOptions inRange;
    inRange.major = major;
    Harness h(std::nullopt, inRange);
    h.store(held(rangeLease(h, 3, 2), "KEY", std::nullopt));
    EXPECT_EQ(h.license().status(), licensed("KEY")) << major;
  }
  HarnessOptions later;
  later.major = 41;
  Harness open(std::nullopt, later);
  open.store(held(rangeLease(open, keygrant::kOpenMajor, 2), "KEY", std::nullopt));
  EXPECT_EQ(open.license().status().state, LicenseState::Licensed);
  Harness renewed;
  expiredLease(renewed, "KEY");
  const std::string fresh = rangeLease(renewed, 2, 2);
  renewed.http->reply("validate", 200, leaseAnswer(fresh));
  EXPECT_EQ(renewed.license().status(), status(LicenseState::Invalid, LicenseReason::Outdated, "KEY"));
  EXPECT_EQ(renewed.stored()->lease, fresh);
}

/** A device on major 3 holding a valid open lease, last heard by the server on 2. */
Harness& onNewMajor(Harness& h, std::int64_t ttl = MONTH_S, const char* model = "perpetual") {
  LeaseSpec spec;
  spec.major = keygrant::kOpenMajor;
  spec.ttlSeconds = ttl;
  spec.model = model;
  StoredState state = held(h.sign(spec), KEY, 2);
  h.store(state);
  return h;
}

HarnessOptions major3() {
  HarnessOptions o;
  o.major = 3;
  return o;
}

TEST(NewerMajor, IsReportedOnAnOpenLeaseAtOnceAndNotedAsHeard) {
  Harness h(std::nullopt, major3());
  onNewMajor(h);
  LeaseSpec spec;
  spec.major = keygrant::kOpenMajor;
  const std::string fresh = h.sign(spec);
  h.http->reply("validate", 200, leaseAnswer(fresh));
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.http->bodyOf("validate")["major"], 3);
  EXPECT_EQ(h.stored()->lease, fresh);
  EXPECT_EQ(h.stored()->checkedMajor, 3);
  // The server heard it: not asked again.
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
}

TEST(NewerMajor, KeepsTheValidLeaseWhenTheCheckCannotReachTheServer) {
  Harness h(std::nullopt, major3());
  onNewMajor(h);
  const std::string lease = *h.stored()->lease;
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_TRUE(h.http->called("validate"));
  EXPECT_EQ(h.stored()->lease, lease);
  EXPECT_EQ(h.stored()->checkedMajor, 2);
  EXPECT_EQ(h.stored()->backoffSince, NOW);
  EXPECT_EQ(h.stored()->backoffMs, 60'000);
}

TEST(NewerMajor, RecordsTheMajorAsHeardOnARefusalKeepsItAndDoesNotAskAgain) {
  const std::vector<std::pair<const char*, LicenseReason>> verdicts = {
      {"revoked", LicenseReason::Revoked},        {"expired", LicenseReason::Revoked},
      {"deactivated", LicenseReason::Revoked},    {"upgrade-required", LicenseReason::Upgrade},
      {"update-required", LicenseReason::Outdated},
  };
  for (const auto& [word, reason] : verdicts) {
    SCOPED_TRACE(word);
    Harness h(std::nullopt, major3());
    onNewMajor(h);
    h.http->reply("validate", 403, {{"error", word}});
    EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, reason, KEY));
    EXPECT_EQ(h.stored()->checkedMajor, 3);
    EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, reason, KEY));
    EXPECT_EQ(h.http->count("validate"), 1);
    if (reason == LicenseReason::Revoked) {
      // A verdict about the key drops the lease.
      EXPECT_EQ(h.stored()->lease, std::nullopt);
      EXPECT_EQ(h.stored()->refusedMajor, std::nullopt);
    } else {
      // About this build: the lease kept, with the major refused.
      EXPECT_TRUE(h.stored()->lease);
      EXPECT_EQ(h.stored()->refusedMajor, 3);
    }
  }
}

TEST(NewerMajor, LetsARollbackToACoveredBuildRunAfterANewerOneWasRefusedAnUpgrade) {
  Harness h(std::nullopt, major3());
  onNewMajor(h);
  h.http->reply("validate", 403, {{"error", "upgrade-required"}});
  EXPECT_EQ(h.licenseFor(4)->status(), status(LicenseState::Invalid, LicenseReason::Upgrade, KEY));
  h.http->fail = true;
  EXPECT_EQ(h.licenseFor(3)->status().state, LicenseState::Licensed);
  EXPECT_EQ(h.licenseFor(4)->status(), status(LicenseState::Invalid, LicenseReason::Upgrade, KEY));
  EXPECT_EQ(h.licenseFor(5)->status(), status(LicenseState::Invalid, LicenseReason::Upgrade, KEY));
  EXPECT_EQ(h.http->count("validate"), 1);
}

TEST(NewerMajor, LetsAnUpdateToACoveredBuildRunAfterAnOlderOneWasRefusedAsOutOfDate) {
  Harness h(std::nullopt, major3());
  onNewMajor(h);
  h.http->reply("validate", 403, {{"error", "update-required"}});
  EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, LicenseReason::Outdated, KEY));
  h.http->fail = true;
  EXPECT_EQ(h.licenseFor(3)->status(), status(LicenseState::Invalid, LicenseReason::Outdated, KEY));
  EXPECT_EQ(h.licenseFor(4)->status().state, LicenseState::Licensed);
  EXPECT_EQ(h.licenseFor(2)->status(), status(LicenseState::Invalid, LicenseReason::Outdated, KEY));
}

TEST(NewerMajor, KeepsACancelledSubscriberRefusedOfflineIncluded) {
  Harness h(std::nullopt, major3());
  onNewMajor(h, 7 * 24 * 3600, "subscription");
  h.http->reply("validate", 403, {{"error", "expired"}});
  const LicenseStatus revoked = status(LicenseState::Invalid, LicenseReason::Revoked, KEY);
  EXPECT_EQ(h.license().status(), revoked);
  EXPECT_EQ(h.license().status(), revoked);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), revoked);
  EXPECT_EQ(h.license().refresh(), revoked);
}

TEST(NewerMajor, HealsARefusedKeyWhenTheServerLicensesItAgain) {
  Harness h(std::nullopt, major3());
  onNewMajor(h);
  h.http->reply("validate", 403, {{"error", "expired"}});
  EXPECT_EQ(h.license().status().state, LicenseState::Invalid);
  h.setClock(NOW + 3'600'000);
  LeaseSpec spec;
  spec.major = keygrant::kOpenMajor;
  h.http->reply("validate", 200, leaseAnswer(h.sign(spec)));
  EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
  EXPECT_EQ(h.stored()->refused, std::nullopt);
  h.http->fail = true;
  EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
}

TEST(NewerMajor, KeepsTheLeaseOnAnAmbiguousAnswerAndRecordsNoRefusal) {
  Harness h(std::nullopt, major3());
  onNewMajor(h);
  h.http->reply("validate", 404, {{"error", "unknown-product"}});
  EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
  EXPECT_EQ(h.stored()->refused, std::nullopt);
  EXPECT_TRUE(h.stored()->lease);
}

TEST(NewerMajor, StartsAfreshOnANewActivationAfterARefusalAndOnADeactivate) {
  Harness h(std::nullopt, major3());
  onNewMajor(h);
  h.http->reply("validate", 403, {{"error", "deactivated"}});
  EXPECT_EQ(h.license().status().state, LicenseState::Invalid);
  LeaseSpec spec;
  spec.aid = "act_2";
  spec.major = keygrant::kOpenMajor;
  h.http->reply("activate", 200, {{"lease", h.sign(spec)}, {"activationId", "act_2"}});
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.stored()->refused, std::nullopt);
  h.http->fail = true;
  EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
  StoredState refused = *h.stored();
  refused.refused = Refusal::Revoked;
  h.store(refused);
  h.license().deactivate();
  EXPECT_EQ(h.stored()->refused, std::nullopt);
  EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
}

TEST(NewerMajor, RecordsTheMajorAsHeardOnAnAnswerItCannotUseAndDoesNotAskAgain) {
  Harness h(std::nullopt, major3());
  onNewMajor(h);
  h.http->reply("validate", 200, {{"lease", "not.a.lease"}});
  EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
  EXPECT_EQ(h.stored()->checkedMajor, 3);
}

TEST(NewerMajor, BacksOffAfterNoAnswerAndAsksAgainOnceTheIntervalHasPassed) {
  Harness h(std::nullopt, major3());
  onNewMajor(h);
  h.http->fail = true;
  EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
  h.setClock(NOW + 60'000 - 1);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
  h.setClock(NOW + 60'000);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 2);
}

TEST(NewerMajor, TreatsA5xxOr429AsNoVerdictAndWaitsTheHourTheServerDidAnswer) {
  for (const int code : {503, 429}) {
    Harness h(std::nullopt, major3());
    onNewMajor(h);
    h.http->reply("validate", code, {{"error", "busy"}});
    EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
    EXPECT_EQ(h.stored()->checkedMajor, 2);
    EXPECT_EQ(h.stored()->backoffMs, 3'600'000);
    h.setClock(NOW + 60'000);
    h.license().status();
    EXPECT_EQ(h.http->count("validate"), 1) << code;
    h.setClock(NOW + 3'600'000);
    h.license().status();
    EXPECT_EQ(h.http->count("validate"), 2) << code;
  }
}

TEST(Refusals, AreAskedAgainOnlyOnceTheWaitIsOverAndAtOnceOnARefresh) {
  Harness h(std::nullopt, major3());
  onNewMajor(h);
  h.http->reply("validate", 403, {{"error", "revoked"}});
  EXPECT_EQ(h.license().status().state, LicenseState::Invalid);
  EXPECT_EQ(h.http->count("validate"), 1);
  h.setClock(NOW + 3'600'000 - 1);
  EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, LicenseReason::Revoked, KEY));
  EXPECT_EQ(h.http->count("validate"), 1);
  EXPECT_EQ(h.license().refresh().state, LicenseState::Invalid);
  EXPECT_EQ(h.http->count("validate"), 2);
  // The refresh's refusal starts the wait afresh.
  h.setClock(NOW + 3'600'000 - 1 + 3'600'000 - 1);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 2);
  h.setClock(NOW + 3'600'000 - 1 + 3'600'000);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 3);
}

TEST(Refusals, AnExpiredLeaseIsAskedAgainAfterTheShortWaitAndALeaseEndsTheWait) {
  Harness h;
  expiredLease(h);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
  EXPECT_EQ(h.stored()->backoffSince, NOW);
  EXPECT_EQ(h.stored()->backoffMs, 60'000);
  h.setClock(NOW + 60'000 - 1);
  EXPECT_EQ(h.license().status().state, LicenseState::Expired);
  EXPECT_EQ(h.http->count("validate"), 1);
  h.setClock(NOW + 60'000);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 2);
  h.license().refresh();
  EXPECT_EQ(h.http->count("validate"), 3);
  h.http->fail = false;
  h.http->reply("validate", 200, leaseAnswer(h.sign(KEY, "perpetual", MONTH_S, NOW + 60'000)));
  EXPECT_EQ(h.license().refresh().state, LicenseState::Licensed);
  EXPECT_EQ(h.stored()->backoffSince, std::nullopt);
}

TEST(Refusals, StillWaitTheHourAfterARefusalTheShortWaitNotwithstanding) {
  Harness h;
  expiredLease(h);
  h.http->reply("validate", 403, {{"error", "revoked"}});
  EXPECT_EQ(h.license().status().reason, LicenseReason::Revoked);
  h.setClock(NOW + 60'000);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
  h.setClock(NOW + 3'600'000);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 2);
}

TEST(SaveFailures, LeaveTheServersAnswerStandingAndFailNothingButActivateAndDeactivate) {
  Harness refused;
  const std::string lease = refused.sign(KEY, "perpetual", MONTH_S);
  refused.store(held(lease));
  refused.http->reply("validate", 403, {{"error", "revoked"}});
  refused.storage->failWrites = true;
  EXPECT_EQ(refused.license().refresh(), status(LicenseState::Invalid, LicenseReason::Revoked, KEY));
  EXPECT_GT(refused.storage->writes(), 0);
  EXPECT_EQ(refused.stored()->lease, lease);

  Harness renewed;
  const std::string old = renewed.sign(KEY, "perpetual", 3600, NOW - 2 * DAY);
  renewed.store(held(old));
  const std::string fresh = renewed.sign(KEY, "perpetual", MONTH_S);
  renewed.http->reply("validate", 200, leaseAnswer(fresh));
  renewed.storage->failWrites = true;
  EXPECT_EQ(renewed.license().status(), licensed(KEY));
  EXPECT_EQ(renewed.stored()->lease, old);
  renewed.storage->failWrites = false;
  EXPECT_EQ(renewed.license().refresh(), licensed(KEY));
  EXPECT_EQ(renewed.stored()->lease, fresh);

  Harness nothing;
  nothing.store(held(nothing.sign(KEY, "perpetual", 3600, NOW - 2 * DAY)));
  nothing.http->fail = true;
  nothing.storage->failWrites = true;
  EXPECT_EQ(nothing.license().status(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
}

TEST(Status, IgnoresAStoredRefusalWhenNoKeyIsHeldAndAnswersTheTrial) {
  // A keyless trial state carrying a stale refusal: nothing it refused is held.
  for (const std::optional<std::string>& key : {std::optional<std::string>(), std::optional<std::string>("")}) {
    Harness h;
    StoredState state;
    state.key = key;
    state.refused = keygrant::Refusal::Revoked;
    state.trialStart = NOW - 2 * DAY;
    state.trialEndsAt = NOW + 5 * DAY;
    state.trialLease = h.sign("trial:fp-test", "trial", 5 * 24 * 3600);
    state.lastSeen = NOW;
    h.store(state);
    const LicenseStatus s = h.license().status();
    EXPECT_EQ(s.state, LicenseState::Trial);
    EXPECT_EQ(s.trialDaysLeft, 5);
    EXPECT_FALSE(h.http->called("validate"));
  }
}

TEST(Status, IsUnlicensedWithATrialsEndOrLeaseButNoStart) {
  // A trial is known by its start: an end or a lease without one starts nothing.
  StoredState endOnly;
  endOnly.trialEndsAt = NOW + 5 * DAY;
  Harness a(endOnly);
  EXPECT_EQ(a.license().status(), status(LicenseState::Unlicensed));

  Harness b;
  StoredState leaseOnly;
  leaseOnly.trialLease = b.sign("trial:fp-test", "trial", 7 * 24 * 3600);
  leaseOnly.trialEndsAt = NOW + 5 * DAY;
  leaseOnly.lastSeen = NOW;
  b.store(leaseOnly);
  EXPECT_EQ(b.license().status(), status(LicenseState::Unlicensed));
}

TEST(DeepReplies, AreAnsweredHoweverDeepTheBodyNestsNeverACrash) {
  // A body nested tens of thousands deep, well under the 1 MiB cap: what the
  // server (or anything between) sends is moved, never copied, since copying
  // JSON recurses as deep as it nests.
  for (const std::size_t depth : kDeepNesting) {
    SCOPED_TRACE(depth);
    const std::string deep = nestedArrays(depth);
    Harness h;
    h.store(held(h.sign(KEY, "perpetual", 3600, NOW - 2 * DAY)));
    for (const char* action : {"validate", "activate", "trial", "deactivate"}) {
      h.http->on(action, [deep](const json&) { return keygrant::HttpResponse{200, deep}; });
    }
    // No lease to use, no verdict: the lease's own end stands.
    EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
    EXPECT_EQ(h.license().refresh(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
    // Neither an activation nor a trial, but an answer.
    EXPECT_EQ(h.license().activate(KEY), failed(ActivateError::Network));
    EXPECT_EQ(h.license().startTrial(), status(LicenseState::Unlicensed));
    EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
    // A refusal's status with a body as deep: no verdict either.
    Harness refused;
    refused.store(held(refused.sign(KEY, "perpetual", 3600, NOW - 2 * DAY)));
    refused.http->on("validate", [deep](const json&) { return keygrant::HttpResponse{403, deep}; });
    EXPECT_EQ(refused.license().refresh(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
  }
}

TEST(NeverThrows, StatusRefreshAndStartTrialAnswerUnknownWhenTheClockThrows) {
  // An app awaits status() at launch: a clock adapter that throws must not take it down.
  Harness h;
  h.store(held(h.sign(KEY, "perpetual", MONTH_S)));
  h.http->reply("validate", 200, leaseAnswer(h.sign(KEY, "perpetual", MONTH_S)));
  h.http->reply("trial", 200, {{"trialStart", NOW}, {"trialEndsAt", NOW + 7 * DAY}});
  h.clock->failing = true;
  const LicenseStatus unknown = status(LicenseState::Unknown, LicenseReason::Storage);
  EXPECT_EQ(h.license().status(), unknown);
  EXPECT_EQ(h.license().refresh(), unknown);
  EXPECT_EQ(h.license().startTrial(), unknown);
  // And once it reads again, the licence is as it was.
  h.clock->failing = false;
  EXPECT_EQ(h.license().status(), licensed(KEY));
}

}  // namespace
}  // namespace kgtest
