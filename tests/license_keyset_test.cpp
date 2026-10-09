// A key set: a lease signed by a key the build does not carry, the ids of the keys reported at
// each check-in; and has() and require(), read offline from the stored lease.
#include <string>

#include "support.hpp"

namespace kgtest {
namespace {

const std::int64_t MONTH_S = 30 * 24 * 3600;
const std::int64_t DAY_MS = 86'400'000;

/** A licence lease for KEY granting `ent`. */
ordered_json claimsGranting(const ordered_json& ent) {
  LeaseSpec spec;
  spec.ttlSeconds = MONTH_S;
  spec.extra["ent"] = ent;
  return claimsOf(spec);
}

TEST(KeySet, AnswersAStoredLeaseSignedByAKeyTheBuildDoesNotCarryAsUnknownKeyWithTheKey) {
  const TestKey next;
  Harness h(held(next.signNamed(claimsGranting(ordered_json::object()), next.kid())));
  h.http->fail = true;
  LicenseStatus expected;
  expected.state = LicenseState::Invalid;
  expected.reason = LicenseReason::UnknownKey;
  expected.key = KEY;
  EXPECT_EQ(h.license().status(), expected);
}

TEST(KeySet, RefusesAnActivationWhoseLeaseNamesAKeyTheBuildDoesNotCarry) {
  const TestKey next;
  Harness h;
  h.http->reply("activate", 200, {{"lease", next.signNamed(claimsGranting(ordered_json::object()), next.kid())}, {"activationId", "act_1"}});
  EXPECT_EQ(h.license().activate(KEY), ActivateResult::failure(ActivateError::InvalidLease));
  EXPECT_FALSE(h.stored() && h.stored()->lease);
}

TEST(KeySet, ActivatesOnALeaseTheSecondKeyOfItsSetSignedAndReportsBothIds) {
  const TestKey next;
  Harness h;
  keygrant::LicenseConfig both = h.config;
  both.publicJwk = "{\"keys\":[" + h.key.publicJwk() + "," + next.publicJwk() + "]}";
  keygrant::License app(both, h.timings);
  h.http->reply("activate", 200, {{"lease", next.signNamed(claimsGranting(ordered_json::object()), next.kid())}, {"activationId", "act_1"}});
  EXPECT_EQ(app.activate(KEY), ActivateResult::success());
  EXPECT_EQ(h.http->bodyOf("activate")["kids"], json::array({h.key.kid(), next.kid()}));
  EXPECT_EQ(app.status(), licensed(KEY));
}

TEST(KeySet, VerifiesALeaseNamingTheKeyItCarries) {
  Harness h;
  h.store(held(h.key.signNamed(claimsGranting(ordered_json::object()), h.key.kid())));
  EXPECT_EQ(h.license().status(), licensed(KEY));
}

const ordered_json kEnt = {{"cloud", true}, {"beta", false}, {"seats", 0}, {"devices", 5}, {"edition", "pro"}, {"blank", ""}};

TEST(Has, AnswersFromTheStoredLeaseAFlagOnANumberNotZeroATextNotEmpty) {
  Harness h;
  h.store(held(h.key.sign(claimsGranting(kEnt))));
  EXPECT_TRUE(h.license().has("cloud"));
  EXPECT_FALSE(h.license().has("beta"));
  EXPECT_FALSE(h.license().has("seats"));
  EXPECT_TRUE(h.license().has("devices"));
  EXPECT_TRUE(h.license().has("edition"));
  EXPECT_FALSE(h.license().has("blank"));
  EXPECT_FALSE(h.license().has("export"));
  EXPECT_FALSE(h.license().has("Cloud"));
}

TEST(Has, NeverGoesOnlineAndNeverWrites) {
  Harness h;
  h.store(held(h.key.sign(claimsGranting(kEnt))));
  // A lease near its end, which status() would renew.
  h.setClock(NOW + 29 * DAY_MS);
  const int writes = h.storage->writes();
  EXPECT_TRUE(h.license().has("cloud"));
  EXPECT_TRUE(h.http->actions().empty());
  EXPECT_EQ(h.storage->writes(), writes);
}

TEST(Has, GrantsNothingOnALeaseThatDoesNotLicenseThisBuildNorWithNoLicence) {
  Harness h;
  h.store(held(h.key.sign(claimsGranting(kEnt))));
  h.setClock(NOW + 31 * DAY_MS);
  EXPECT_FALSE(h.license().has("cloud"));
  h.store(std::nullopt);
  EXPECT_FALSE(h.license().has("cloud"));
}

TEST(Has, GrantsNothingAndDoesNotThrowWhileTheStateCannotBeRead) {
  Harness h;
  h.store(held(h.key.sign(claimsGranting(kEnt))));
  h.storage->failReads = true;
  EXPECT_FALSE(h.license().has("cloud"));
}

TEST(Has, GrantsNothingOverAStoredRefusalThoughTheLeaseItKeptStillVerifies) {
  Harness h;
  // The server refused this build's major: the lease is kept for builds it covers.
  StoredState upgrade = held(h.key.sign(claimsGranting(kEnt)));
  upgrade.refused = keygrant::Refusal::Upgrade;
  upgrade.refusedMajor = 1;
  h.store(upgrade);
  EXPECT_FALSE(h.license().has("cloud"));
  // Revoked: the refusal stands over any lease still held.
  StoredState revoked = held(h.key.sign(claimsGranting(kEnt)));
  revoked.refused = keygrant::Refusal::Revoked;
  h.store(revoked);
  EXPECT_FALSE(h.license().has("cloud"));
}

TEST(Has, GrantsDuringARunningTrialWhatItsTrialLeaseSignsAndAsksNothing) {
  Harness h;
  LeaseSpec spec;
  spec.sub = "trial:fp-test";
  spec.model = "trial";
  spec.lim = 1;
  spec.major = 1'000'000;
  spec.ttlSeconds = 5 * 24 * 3600;
  spec.extra["ent"] = {{"cloud", true}};
  StoredState state;
  state.trialStart = NOW - 2 * DAY_MS;
  state.trialEndsAt = NOW + 5 * DAY_MS;
  state.trialLease = h.sign(spec);
  state.lastSeen = NOW;
  h.store(state);
  EXPECT_TRUE(h.license().has("cloud"));
  EXPECT_FALSE(h.license().has("export"));
  EXPECT_TRUE(h.http->actions().empty());
}

TEST(Require, PassesForAGrantedCodeAndThrowsNamingTheCodeOtherwise) {
  Harness h;
  h.store(held(h.key.sign(claimsGranting(kEnt))));
  EXPECT_NO_THROW(h.license().require("cloud"));
  try {
    h.license().require("seats");
    ADD_FAILURE() << "require granted seats";
  } catch (const keygrant::MissingEntitlement& missing) {
    EXPECT_EQ(missing.entitlement(), "seats");
  }
}

}  // namespace
}  // namespace kgtest
