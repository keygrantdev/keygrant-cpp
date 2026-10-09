// The lease's entitlements in every status, and the value type that holds one.
#include <cstddef>
#include <future>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "support.hpp"

namespace kgtest {
namespace {

using namespace std::chrono_literals;
using keygrant::Entitlement;
using Entitlements = std::map<std::string, Entitlement>;

const std::int64_t MONTH_S = 30 * 24 * 3600;

Entitlements pro() { return {{"edition", "pro"}, {"seats", 5}, {"beta", false}}; }
ordered_json proJson() { return {{"edition", "pro"}, {"seats", 5}, {"beta", false}}; }

/** A lease as `spec` says, its `ent` as given: any JSON, malformed or not (the server never signs one; an SDK still reads one). */
std::string granting(const Harness& h, const ordered_json& ent, LeaseSpec spec = {}) {
  spec.extra["ent"] = ent;
  return h.sign(spec);
}

/** A licence lease for KEY, signed `now` and ending `ttlSeconds` after. */
LeaseSpec leaseFor(std::int64_t ttlSeconds = MONTH_S, std::int64_t now = NOW) {
  LeaseSpec spec;
  spec.ttlSeconds = ttlSeconds;
  spec.now = now;
  return spec;
}

LicenseStatus licensedWith(Entitlements entitlements, bool test = false) {
  LicenseStatus s = licensed(KEY, test);
  s.entitlements = std::move(entitlements);
  return s;
}

void activated(Harness& h, const std::string& lease) {
  h.http->reply("activate", 200, {{"lease", lease}, {"activationId", "act_1"}});
  ASSERT_EQ(h.license().activate(KEY), ok());
}

TEST(Entitlements, AreHandedToTheAppOnceActivated) {
  Harness h;
  activated(h, granting(h, proJson()));
  const LicenseStatus status = h.license().status();
  EXPECT_EQ(status, licensedWith(pro()));
  // As an app reads one.
  const auto edition = status.entitlements.find("edition");
  ASSERT_TRUE(edition != status.entitlements.end());
  EXPECT_EQ(edition->second, "pro");
  EXPECT_EQ(edition->second.text(), "pro");
  EXPECT_EQ(status.entitlements.at("seats").number(), 5.0);
  EXPECT_EQ(status.entitlements.at("beta").flag(), false);
}

TEST(Entitlements, AreNoneWhenTheLeaseGrantsNoneAndTheStatusIsWhatItWasBefore) {
  // As `test` is false: an app comparing whole statuses sees no change.
  Harness h;
  activated(h, granting(h, ordered_json::object()));
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_TRUE(h.license().status().entitlements.empty());
  // A lease with no `ent` claim at all reads the same.
  ordered_json claims = claimsOf(LeaseSpec{});
  claims.erase("ent");
  h.store(held(h.key.sign(claims)));
  EXPECT_EQ(h.license().status(), licensed(KEY));
}

TEST(Entitlements, KeepTheGoodMembersOfAMalformedEntAndTheLicenceItRidesOn) {
  Harness h;
  const ordered_json ent = {{"edition", "pro"}, {"nested", {{"tier", "gold"}}}, {"none", nullptr},
                            {"list", {1}},      {"seats", 2}};
  h.store(held(granting(h, ent)));
  EXPECT_EQ(h.license().status(), licensedWith({{"edition", "pro"}, {"seats", 2}}));
  EXPECT_FALSE(h.http->called("validate"));
}

TEST(Entitlements, ReadAnEntThatIsNotAnObjectAsNoneNeverAsARefusal) {
  for (const ordered_json& ent : {ordered_json::array({"pro"}), ordered_json(nullptr), ordered_json("pro"),
                                  ordered_json(42), ordered_json(true)}) {
    Harness h;
    h.store(held(granting(h, ent)));
    EXPECT_EQ(h.license().status(), licensed(KEY)) << ent.dump();
  }
}

TEST(Entitlements, AreTakenFromTheRenewedLeaseNotTheOneItReplaced) {
  // From an EXPIRED lease, so the status can only come from the renewal.
  Harness h;
  activated(h, granting(h, {{"edition", "basic"}}, leaseFor(60)));
  const std::int64_t later = NOW + 2 * 3'600'000;
  h.setClock(later);
  h.http->reply("validate", 200, {{"lease", granting(h, proJson(), leaseFor(MONTH_S, later))}});
  EXPECT_EQ(h.license().status(), licensedWith(pro()));
  EXPECT_TRUE(h.http->called("validate"));
}

TEST(Entitlements, OnRefreshAreTheNewLeasesWhenTheServerAnswersAndTheHeldLeasesWhenItCannot) {
  Harness h;
  activated(h, granting(h, {{"edition", "basic"}}));
  h.http->reply("validate", 200, {{"lease", granting(h, proJson())}});
  EXPECT_EQ(h.license().refresh(), licensedWith(pro()));
  h.http->fail = true;
  EXPECT_EQ(h.license().refresh(), licensedWith(pro()));
  EXPECT_EQ(h.http->actions(), (std::vector<std::string>{"activate", "validate", "validate"}));
}

TEST(Entitlements, RideBesideATestLicencesFlag) {
  Harness h;
  LeaseSpec spec;
  spec.extra = {{"test", true}};
  activated(h, granting(h, proJson(), spec));
  EXPECT_EQ(h.license().status(), licensedWith(pro(), true));
}

TEST(Entitlements, AreNoneOnAStatusThatDoesNotLicenseThoughTheLeaseGrantsThem) {
  HarnessOptions two;
  two.major = 2;
  Harness h(std::nullopt, two);
  h.http->fail = true;
  // A newer build than the lease covers.
  h.store(held(granting(h, proJson())));
  EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, LicenseReason::Upgrade, KEY));
  // A lease past its end, the server out of reach.
  LeaseSpec ended = leaseFor(3600, NOW - 2 * DAY);
  ended.major = 2;
  h.store(held(granting(h, proJson(), ended)));
  EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
  // A lease for another key.
  LeaseSpec another;
  another.sub = "SLUICE-ZZZZ-ZZZZ-ZZZZ-ZZZZ";
  another.major = 2;
  h.store(held(granting(h, proJson(), another)));
  EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, LicenseReason::Tampered));
}

TEST(Entitlements, AreNoneOnARefusalTheServerGaveThoughTheLeaseItKeepsGrantsThem) {
  // An upgrade refusal keeps the lease (a rollback runs offline on it), so the
  // entitlements are still on the device: the refusal must not carry them.
  Harness h;
  activated(h, granting(h, proJson()));
  h.http->reply("validate", 403, {{"error", "upgrade-required"}});
  EXPECT_EQ(h.license().refresh(), status(LicenseState::Invalid, LicenseReason::Upgrade, KEY));
  EXPECT_TRUE(h.stored()->lease);
  // The stored refusal, offline: none either.
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, LicenseReason::Upgrade, KEY));
}

TEST(Entitlements, AreNoneOnAStoredRefusalAnsweredBehindACallThatHasNotFinished) {
  HarnessOptions quick;
  quick.httpTimeout = Millis{100};
  quick.timings = shortTimings();
  Harness h(std::nullopt, quick);
  StoredState state = held(granting(h, proJson()));
  state.refused = keygrant::Refusal::Upgrade;
  state.refusedMajor = 1;
  h.store(state);
  h.http->fail = true;
  h.storage->hangNextRead();
  auto stuck = std::async(std::launch::async, [&] { return h.license().status(); });
  ASSERT_TRUE(h.storage->readGate().waitReached(5s));
  const LicenseStatus refused = status(LicenseState::Invalid, LicenseReason::Upgrade, KEY);
  EXPECT_EQ(h.license().status(), stalled(refused));
  h.storage->readGate().open();
  EXPECT_EQ(stuck.get(), refused);
}

TEST(Entitlements, KeepTheHeldLeasesWhenAnEarlyRenewalGetsNoAnswer) {
  // Five days left of thirty: due to renew early, and the server is out of reach.
  Harness h;
  h.store(held(granting(h, proJson(), leaseFor(MONTH_S, NOW - 25 * DAY))));
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensedWith(pro()));
  EXPECT_EQ(h.http->actions(), std::vector<std::string>{"validate"});
}

TEST(Entitlements, AreATrialLeasesFromStartTrialAndEveryStatusAfter) {
  Harness h;
  LeaseSpec trial = leaseFor(5 * 24 * 3600);
  trial.sub = "trial:fp-test";
  trial.model = "trial";
  h.http->reply("trial", 200,
                {{"trialStart", NOW - 2 * DAY}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", granting(h, proJson(), trial)}});
  LicenseStatus expected = status(LicenseState::Trial);
  expected.trialEndsAt = NOW + 5 * DAY;
  expected.trialDaysLeft = 5;
  expected.entitlements = pro();
  EXPECT_EQ(h.license().startTrial(), expected);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), expected);
}

TEST(Entitlements, AreNoneOnceATrialsLeaseNoLongerRunsIt) {
  Harness h;
  LeaseSpec trial = leaseFor(24 * 3600);
  trial.sub = "trial:fp-test";
  trial.model = "trial";
  StoredState state;
  state.trialStart = NOW - 6 * DAY;
  state.trialEndsAt = NOW + DAY;
  state.trialLease = granting(h, proJson(), trial);
  state.lastSeen = NOW;
  h.store(state);
  h.setClock(NOW + 2 * DAY);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Trial));
}

TEST(Entitlements, StillRideAStalledAnswerFromTheLeaseHeld) {
  HarnessOptions quick;
  quick.httpTimeout = Millis{100};
  quick.timings = shortTimings();
  Harness h(std::nullopt, quick);
  h.store(held(granting(h, proJson())));
  h.storage->hangNextRead();
  auto stuck = std::async(std::launch::async, [&] { return h.license().status(); });
  ASSERT_TRUE(h.storage->readGate().waitReached(5s));
  // Answered from storage behind the stuck call, and saying so.
  EXPECT_EQ(h.license().status(), stalled(licensedWith(pro())));
  h.storage->readGate().open();
  EXPECT_EQ(stuck.get(), licensedWith(pro()));
}

TEST(Entitlements, CannotBeChangedThroughAStatusWhichLeavesTheNextAsTheLeaseSays) {
  // A status holds its own copy: what the app does to it reaches nothing else.
  Harness h;
  h.store(held(granting(h, proJson())));
  LicenseStatus first = h.license().status();
  first.entitlements.insert_or_assign("edition", "studio");
  first.entitlements.erase("seats");
  EXPECT_EQ(h.license().status().entitlements, pro());
}

// --- the value type ---------------------------------------------------------------

TEST(Entitlement, IsAFlagANumberOrATextAsWritten) {
  // A string literal is a text (never the bool a bare variant may make of it), any number a double.
  const Entitlement edition = "pro";
  EXPECT_EQ(edition.kind(), Entitlement::Kind::Text);
  EXPECT_TRUE(edition.isText());
  EXPECT_EQ(edition.text(), "pro");
  EXPECT_EQ(edition.flag(), std::nullopt);
  EXPECT_EQ(edition.number(), std::nullopt);
  const Entitlement seats = 5;
  EXPECT_EQ(seats.kind(), Entitlement::Kind::Number);
  EXPECT_TRUE(seats.isNumber());
  EXPECT_EQ(seats.number(), 5.0);
  EXPECT_EQ(seats.text(), std::nullopt);
  const Entitlement beta = false;
  EXPECT_EQ(beta.kind(), Entitlement::Kind::Flag);
  EXPECT_TRUE(beta.isFlag());
  EXPECT_EQ(beta.flag(), false);
  EXPECT_EQ(beta.number(), std::nullopt);
  EXPECT_EQ(Entitlement(std::string("studio")).text(), "studio");
  EXPECT_EQ(Entitlement(std::int64_t{1} << 40).number(), 1099511627776.0);
}

TEST(Entitlement, ComparesByKindAndValueFiveAndFivePointZeroAlike) {
  EXPECT_EQ(Entitlement(5), Entitlement(5.0));
  EXPECT_EQ(Entitlement(0.5), 0.5);
  EXPECT_EQ(Entitlement("pro"), "pro");
  EXPECT_NE(Entitlement(1), Entitlement(true));
  EXPECT_NE(Entitlement("5"), Entitlement(5));
  EXPECT_NE(Entitlement("true"), Entitlement(true));
  // And a status compares its entitlements by content.
  EXPECT_EQ(licensedWith({{"seats", 5}}), licensedWith({{"seats", 5.0}}));
  EXPECT_NE(licensedWith({{"seats", 5}}), licensedWith({{"seats", 6}}));
  EXPECT_NE(licensedWith({{"seats", 5}}), licensed(KEY));
}

TEST(Entitlement, PrintsAsJsonWritesItAndSoDoesAStatusCarryingSome) {
  std::ostringstream out;
  out << licensedWith(pro());
  EXPECT_EQ(out.str(),
            R"({ state: licensed, key: SLUICE-AAAA-BBBB-CCCC-DDDD, entitlements: { "beta": false, "edition": "pro", "seats": 5.0 } })");
}

/** Whether an entitlement compares with a `T` at all (`entitlement == t` compiles). */
template <class T, class = void>
struct ComparesWithEntitlement : std::false_type {};
template <class T>
struct ComparesWithEntitlement<T, std::void_t<decltype(std::declval<const Entitlement&>() == std::declval<T>())>>
    : std::true_type {};

/** A `T` is neither made an entitlement nor compared with one: the program does not compile. */
template <class T>
constexpr bool kRefused = !std::is_constructible_v<Entitlement, T> && !std::is_convertible_v<T, Entitlement> &&
                          !ComparesWithEntitlement<T>::value;

/** An enum of each kind: an unscoped one converts to bool and to int, and neither is an entitlement. */
enum PlainEnum { kPlainEnum };
enum class ScopedEnum { kScopedEnum };
struct Holder {
  int member;
};

TEST(Entitlement, IsNeverMadeFromAWideOrUtfLiteralAnEnumOrAPointerThatIsNotText) {
  // Checked as the program compiles: each would otherwise be the flag true
  // (through the conversion to bool), so `entitlement == L"pro"` would be
  // false for a granted "pro", with no diagnostic.
  static_assert(kRefused<decltype(L"pro")> && kRefused<decltype(u"pro")> && kRefused<decltype(U"pro")> &&
                    kRefused<const wchar_t*> && kRefused<const char16_t*> && kRefused<const char32_t*> &&
                    kRefused<const unsigned char*> && kRefused<const void*> && kRefused<int*>,
                "a pointer that is not narrow text is no entitlement");
  static_assert(kRefused<PlainEnum> && kRefused<ScopedEnum> && kRefused<std::nullptr_t> &&
                    kRefused<int Holder::*>,
                "an enum, a null pointer and a member pointer are no entitlement");
  // A C++20 u8 literal and char8_t: checked by tests/package/entitlement_literals.cpp, which every
  // test build compiles as C++20 (CMakeLists.txt, keygrant_cxx20_checks), as this file is C++17.
  // A narrow literal, a C string and a std::string still make a text, and compare.
  static_assert(std::is_convertible_v<decltype("pro"), Entitlement> && std::is_convertible_v<char*, Entitlement> &&
                    std::is_convertible_v<const char* const, Entitlement> &&
                    std::is_convertible_v<std::string, Entitlement> && ComparesWithEntitlement<decltype("pro")>::value,
                "narrow text is an entitlement");
  char buffer[] = "pro";
  EXPECT_EQ(Entitlement(buffer).text(), std::optional<std::string>("pro"));
  EXPECT_EQ(Entitlement("pro"), Entitlement(std::string("pro")));
}

TEST(Entitlement, IsNeverMadeFromACharacterWhichIsNoNumber) {
  // Checked as the program compiles: 'p' is not read as its code (112), nor,
  // through bool, as a flag, and `entitlement == 'p'` does not compile.
  static_assert(kRefused<char> && kRefused<signed char> && kRefused<unsigned char> && kRefused<wchar_t> &&
                    kRefused<char16_t> && kRefused<char32_t> && kRefused<const char>,
                "a character is no entitlement");
  // A number, a flag and a text still are, and still compare.
  static_assert(std::is_convertible_v<int, Entitlement> && std::is_convertible_v<double, Entitlement> &&
                    std::is_convertible_v<bool, Entitlement> && std::is_convertible_v<const char*, Entitlement> &&
                    ComparesWithEntitlement<int>::value && ComparesWithEntitlement<const char*>::value,
                "a number, a flag and a text are entitlements");
  // A character cast to a number is that number.
  EXPECT_EQ(Entitlement(static_cast<int>('p')).number(), 112.0);
}

}  // namespace
}  // namespace kgtest
