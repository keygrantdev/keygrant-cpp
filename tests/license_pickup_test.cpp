// Key pickup: what pickUp(email) and pickUp(email, code) send and answer, and the key restoreByEmail activates. The
// rules each piece follows are held to the conformance vectors (pickupAsks, pickupRefusals, pickedKeys, pickupChoices);
// these are the calls around them.
#include <future>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "pickup.hpp"
#include "support.hpp"

namespace kgtest {
namespace {

using namespace std::chrono_literals;
using keygrant::PickedKey;
using keygrant::PickedVersions;
using keygrant::PickUpKeysResult;
using keygrant::PickUpSentResult;
using keygrant::PickupError;
using keygrant::RestoreResult;
using keygrant::detail::kPickupKeysKeepMs;

const std::string kAddress = "Mara@Studio.example";

/// A key as a confirm's answer gives it, with what a test leaves out filled in.
json picked(const std::string& key = KEY, std::int64_t minMajor = 1, std::optional<std::int64_t> maxMajor = 1,
            std::int64_t activeDevices = 0, bool held = false, std::optional<std::string> activationCode = std::nullopt) {
  json out = {{"key", key},
              {"model", "perpetual"},
              {"modelName", "Single"},
              {"state", "active"},
              {"versions", {{"minMajor", minMajor}, {"maxMajor", maxMajor ? json(*maxMajor) : json(nullptr)}, {"upgradeUntil", nullptr}}},
              {"deviceLimit", 1},
              {"activeDevices", activeDevices},
              {"held", held}};
  if (activationCode) out["activationCode"] = *activationCode;
  return out;
}

void confirming(Harness& h, const std::vector<json>& keys) { h.http->reply("confirm", 200, {{"keys", keys}}); }

void activating(Harness& h) {
  h.http->reply("activate", 200, {{"lease", h.sign(KEY, "perpetual", 30 * 24 * 3600)}, {"activationId", "act_1"}});
}

PickUpKeysResult refusedKeys(PickupError error, std::optional<std::int64_t> wait = std::nullopt) {
  return PickUpKeysResult::failure(error, wait);
}

// --- pickUp(email) ---------------------------------------------------------------------------------------

TEST(Pickup, AsksForACodeWithTheAddressTrimmedAndAnswersSent) {
  Harness h;
  h.http->reply("pickup", 200, {{"ok", true}});
  EXPECT_EQ(h.license().pickUp("  Mara@Studio.example\n"), PickUpSentResult::sent());
  EXPECT_EQ(h.http->bodyOf("pickup"), (json{{"email", kAddress}}));
  const std::string url = h.http->urlOf("pickup");
  EXPECT_EQ(url.substr(url.size() - 26), "/v1/products/sluice/pickup");
}

TEST(Pickup, RefusesABlankAddressBeforeItAsksAndAnswersEachRefusalAsItsError) {
  Harness h;
  EXPECT_EQ(h.license().pickUp("   "), PickUpSentResult::failure(PickupError::InvalidEmail));
  EXPECT_EQ(h.license().pickUp(std::string(254, 'a') + "@b.example"), PickUpSentResult::failure(PickupError::InvalidEmail));
  EXPECT_EQ(h.http->count("pickup"), 0);

  h.http->reply("pickup", 403, {{"error", "pickup-disabled"}});
  EXPECT_EQ(h.license().pickUp(kAddress), PickUpSentResult::failure(PickupError::PickupDisabled));
  h.http->reply("pickup", 429, {{"error", "rate-limited"}, {"retryAfter", 120}});
  EXPECT_EQ(h.license().pickUp(kAddress), PickUpSentResult::failure(PickupError::TooManyAttempts, 120));
  h.http->fail = true;
  EXPECT_EQ(h.license().pickUp(kAddress), PickUpSentResult::failure(PickupError::Network));
}

TEST(Pickup, ReadsTheWaitFromTheBodyAndTheWordDecidesNeverTheStatus) {
  Harness h;
  h.http->reply("pickup", 503, {{"error", "unavailable"}, {"retryAfter", 60}});
  EXPECT_EQ(h.license().pickUp(kAddress), PickUpSentResult::failure(PickupError::TryAgain, 60));
  h.http->reply("pickup", 502, {{"error", "wrong-code"}});
  EXPECT_EQ(h.license().pickUp(kAddress), PickUpSentResult::failure(PickupError::WrongCode));
  h.http->reply("pickup", 404, {{"error", "unknown-product"}});
  EXPECT_EQ(h.license().pickUp(kAddress), PickUpSentResult::failure(PickupError::Network));
}

TEST(Pickup, WritesNothingWhenItAsksForACode) {
  Harness h;
  h.http->reply("pickup", 200, {{"ok", true}});
  h.license().pickUp(kAddress);
  EXPECT_EQ(h.storage->writes(), 0);
  EXPECT_EQ(h.stored(), std::nullopt);
}

// --- pickUp(email, code) ---------------------------------------------------------------------------------

TEST(Pickup, SendsTheAddressTheCodeAndThisDevicesFingerprintsAndAnswersTheKeysAsRead) {
  Harness h;
  confirming(h, {json{{"key", KEY}, {"model", "perpetual"}, {"modelName", "Single"}, {"state", "active"}, {"held", true}}});
  const PickUpKeysResult result = h.license().pickUp(kAddress, " 123 456 ");
  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.keys.size(), 1u);
  PickedKey expected;
  expected.key = KEY;
  expected.model = "perpetual";
  expected.modelName = "Single";
  expected.state = "active";
  expected.held = true;
  EXPECT_EQ(result.keys[0], expected);
  const json body = h.http->bodyOf("confirm");
  EXPECT_EQ(body["email"], kAddress);
  EXPECT_EQ(body["code"], "123456");
  EXPECT_EQ(body["fingerprints"], (json{"fp-test"}));
  const std::string url = h.http->urlOf("confirm");
  EXPECT_EQ(url.substr(url.size() - 34), "/v1/products/sluice/pickup/confirm");
}

TEST(Pickup, SendsNoFingerprintsForAProductWhoseDevicesAreNotHeld) {
  HarnessOptions options;
  options.deviceHold = false;
  Harness h(std::nullopt, options);
  confirming(h, {});
  EXPECT_TRUE(h.license().pickUp(kAddress, "123456").ok);
  EXPECT_FALSE(h.http->bodyOf("confirm").contains("fingerprints"));
}

TEST(Pickup, RefusesACodeThatIsNotSixDigitsBeforeItAsksCostingTheAddressNoTry) {
  Harness h;
  for (const std::string code : {"12345", "", "   ", "12345a", "\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93\xEF\xBC\x94\xEF\xBC\x95\xEF\xBC\x96"}) {
    EXPECT_EQ(h.license().pickUp(kAddress, code), refusedKeys(PickupError::WrongCode)) << code;
  }
  // The address is judged first.
  EXPECT_EQ(h.license().pickUp("", "12"), refusedKeys(PickupError::InvalidEmail));
  EXPECT_EQ(h.http->count("confirm"), 0);
}

TEST(Pickup, AnswersTheServersWordsAndA200ThatIsNoPickupAnswerAsNetwork) {
  Harness h;
  const std::vector<std::tuple<int, std::string, PickupError>> words = {
      {403, "wrong-code", PickupError::WrongCode},       {403, "code-expired", PickupError::CodeExpired},
      {403, "pickup-disabled", PickupError::PickupDisabled}, {503, "try-again", PickupError::TryAgain},
      {400, "invalid-request", PickupError::InvalidEmail}};
  for (const auto& [status, word, error] : words) {
    h.http->reply("confirm", status, {{"error", word}});
    EXPECT_EQ(h.license().pickUp(kAddress, "123456"), refusedKeys(error)) << word;
  }
  h.http->reply("confirm", 429, {{"error", "too-many-attempts"}, {"retryAfter", 3000}});
  EXPECT_EQ(h.license().pickUp(kAddress, "123456"), refusedKeys(PickupError::TooManyAttempts, 3000));
  for (const json& body : {json{{"ok", true}}, json{{"keys", "no"}}, json::array()}) {
    h.http->reply("confirm", 200, body);
    EXPECT_EQ(h.license().pickUp(kAddress, "123456"), refusedKeys(PickupError::Network)) << body.dump();
  }
  h.http->fail = true;
  EXPECT_EQ(h.license().pickUp(kAddress, "123456"), refusedKeys(PickupError::Network));
}

TEST(Pickup, WritesNothingWhenItConfirms) {
  Harness h;
  confirming(h, {picked()});
  EXPECT_TRUE(h.license().pickUp(kAddress, "123456").ok);
  EXPECT_EQ(h.storage->writes(), 0);
  EXPECT_EQ(h.stored(), std::nullopt);
}

TEST(Pickup, AsksAgainWithAnotherCodeAndOnceTheKeysAreTenMinutesOld) {
  Harness h;
  confirming(h, {});
  h.license().pickUp(kAddress, "123456");
  h.license().pickUp(kAddress, "654321");
  EXPECT_EQ(h.http->count("confirm"), 2);
  h.license().pickUp(kAddress, "654321");
  EXPECT_EQ(h.http->count("confirm"), 2);
  h.setClock(NOW + kPickupKeysKeepMs);
  h.license().pickUp(kAddress, "654321");
  EXPECT_EQ(h.http->count("confirm"), 3);
  // Another address asks again too.
  h.license().pickUp("other@studio.example", "654321");
  EXPECT_EQ(h.http->count("confirm"), 4);
}

// --- restoreByEmail --------------------------------------------------------------------------------------

TEST(Restore, ConfirmsThenActivatesTheKeyThatFitsWithTheAddressAndStoresTheActivationNeverTheCode) {
  Harness h;
  confirming(h, {picked()});
  activating(h);
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "123456"), RestoreResult::success());
  const json body = h.http->bodyOf("activate");
  EXPECT_EQ(body["key"], KEY);
  EXPECT_EQ(body["email"], kAddress);
  EXPECT_FALSE(body.contains("code"));
  const std::string stored = keygrant::serializeState(*h.stored());
  EXPECT_NE(stored.find(KEY), std::string::npos);
  EXPECT_EQ(stored.find("studio"), std::string::npos);
  EXPECT_EQ(stored.find("Studio"), std::string::npos);
  EXPECT_EQ(stored.find("123456"), std::string::npos);
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.http->actions(), (std::vector<std::string>{"confirm", "activate"}));
}

TEST(Restore, ActivatesTheAddressAsThePickupSentItTrimmed) {
  Harness h;
  confirming(h, {picked()});
  activating(h);
  EXPECT_EQ(h.license().restoreByEmail("  Mara@Studio.example\n", " 123 456 "), RestoreResult::success());
  EXPECT_EQ(h.http->bodyOf("confirm")["email"], kAddress);
  EXPECT_EQ(h.http->bodyOf("confirm")["code"], "123456");
  EXPECT_EQ(h.http->bodyOf("activate")["email"], kAddress);
}

TEST(Restore, ActivatesACodeLockedKeyWithTheCodeThePickupHandedBack) {
  Harness h;
  confirming(h, {picked(KEY, 1, 1, 0, false, "654321")});
  activating(h);
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "123456"), RestoreResult::success());
  const json body = h.http->bodyOf("activate");
  EXPECT_EQ(body["key"], KEY);
  EXPECT_EQ(body["email"], kAddress);
  EXPECT_EQ(body["code"], "654321");
  // Neither the pickup's code nor the lock's is stored.
  const std::string stored = keygrant::serializeState(*h.stored());
  EXPECT_EQ(stored.find("654321"), std::string::npos);
  EXPECT_EQ(stored.find("123456"), std::string::npos);
}

TEST(Restore, ChoosesTheKeyThisDeviceHoldsThenOneWithASeat) {
  const std::string a = "SLUICE-AAAA-AAAA-AAAA-AAAA";
  const std::string b = "SLUICE-BBBB-BBBB-BBBB-BBBB";
  const std::string c = "SLUICE-CCCC-CCCC-CCCC-CCCC";
  Harness h;
  confirming(h, {picked(a, 1, 1, 1), picked(b), picked(c, 1, 1, 1, true)});
  h.http->reply("activate", 409, {{"error", "limit"}});
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "123456"), RestoreResult::activationFailure(ActivateError::Limit));
  EXPECT_EQ(h.http->bodyOf("activate")["key"], c);
  confirming(h, {picked(a, 1, 1, 1), picked(b)});
  h.license().restoreByEmail(kAddress, "654321");
  EXPECT_EQ(h.http->bodiesOf("activate").back()["key"], b);
}

TEST(Restore, AnswersNoKeyForAnAddressWithNoneAndUpgradeOrOutdatedActivatingNothing) {
  HarnessOptions options;
  options.major = 2;
  Harness h(std::nullopt, options);
  confirming(h, {});
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "123456"), RestoreResult::noKeyFound());
  // Keys for an older major: buy an upgrade.
  confirming(h, {picked()});
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "222222"), RestoreResult::activationFailure(ActivateError::Upgrade));
  // Keys that start at a major after this build: update the app.
  confirming(h, {picked(KEY, 3, std::nullopt)});
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "333333"), RestoreResult::activationFailure(ActivateError::Outdated));
  EXPECT_EQ(h.http->count("activate"), 0);
  EXPECT_EQ(h.stored(), std::nullopt);
}

TEST(Restore, AnswersAPickupRefusalAndAnActivationsAsTheyCome) {
  Harness h;
  h.http->reply("confirm", 403, {{"error", "code-expired"}});
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "123456"), RestoreResult::pickupFailure(PickupError::CodeExpired));
  h.http->reply("confirm", 429, {{"error", "too-many-attempts"}, {"retryAfter", 3600}});
  const RestoreResult waiting = h.license().restoreByEmail(kAddress, "123456");
  EXPECT_EQ(waiting.pickupError, PickupError::TooManyAttempts);
  EXPECT_EQ(waiting.retryAfter, 3600);
  // The typed refusals ask nothing.
  EXPECT_EQ(h.license().restoreByEmail("", "123456"), RestoreResult::pickupFailure(PickupError::InvalidEmail));
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "12"), RestoreResult::pickupFailure(PickupError::WrongCode));
  EXPECT_EQ(h.http->count("confirm"), 2);

  confirming(h, {picked()});
  h.http->reply("activate", 409, {{"error", "limit"}});
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "123456"), RestoreResult::activationFailure(ActivateError::Limit));
  h.http->reply("activate", 403, {{"error", "user-code-invalid"}});
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "123456"), RestoreResult::activationFailure(ActivateError::UserCodeInvalid));
  h.http->fail = true;
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "123456"), RestoreResult::activationFailure(ActivateError::Network));
}

TEST(Restore, KeepsTheConfirmedKeysInMemorySoARetryOrARestoreAfterPickUpAsksOnce) {
  Harness h;
  confirming(h, {picked()});
  h.http->reply("activate", 503, {{"error", "unavailable"}});
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "123456"), RestoreResult::activationFailure(ActivateError::Network));
  activating(h);
  EXPECT_EQ(h.license().restoreByEmail(kAddress, " 123 456 "), RestoreResult::success());
  EXPECT_EQ(h.http->count("confirm"), 1);
  // Done with once activated: the same pair asks again.
  EXPECT_TRUE(h.license().pickUp(kAddress, "123456").ok);
  EXPECT_EQ(h.http->count("confirm"), 2);
  // Kept in memory only: nothing of them is stored.
  EXPECT_EQ(keygrant::serializeState(*h.stored()).find("123456"), std::string::npos);
}

TEST(Restore, LeavesTheLicenceThisDeviceHoldsAsItWasWhenARestoreFails) {
  Harness h;
  activating(h);
  EXPECT_EQ(h.license().activate(KEY), ok());
  const StoredState before = *h.stored();
  confirming(h, {picked("SLUICE-OTHR-BBBB-CCCC-DDDD")});
  h.http->reply("activate", 409, {{"error", "limit"}});
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "123456"), RestoreResult::activationFailure(ActivateError::Limit));
  h.http->fail = true;
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "654321"), RestoreResult::pickupFailure(PickupError::Network));
  EXPECT_EQ(*h.stored(), before);
}

// --- behind a call that has not finished -------------------------------------------------------------------

TEST(Restore, AnswersPickupsBesideAStuckCallAndARestoreNetworkAskingNothing) {
  HarnessOptions options;
  options.httpTimeout = Millis{100};
  options.timings = shortTimings();
  Harness h(std::nullopt, options);
  h.store(held(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  h.http->reply("validate", 403, {{"error", "revoked"}});
  h.http->reply("pickup", 200, {{"ok", true}});
  confirming(h, {});
  h.storage->holdNextWrite();
  auto stuck = std::async(std::launch::async, [&] { return h.license().refresh(); });
  ASSERT_TRUE(h.storage->writeGate().waitReached(5s));
  std::this_thread::sleep_for(Millis{300});  // past its grace
  EXPECT_EQ(h.license().pickUp(kAddress), PickUpSentResult::sent());
  EXPECT_TRUE(h.license().pickUp(kAddress, "123456").ok);
  EXPECT_EQ(h.license().restoreByEmail(kAddress, "654321"), RestoreResult::activationFailure(ActivateError::Network));
  EXPECT_EQ(h.http->count("confirm"), 1);
  EXPECT_EQ(h.storage->writes(), 1);
  h.storage->writeGate().open();
  finish(stuck);
}

// --- the pieces ---------------------------------------------------------------------------------------------

TEST(PickupPieces, AskForACodeWithAnAddressAloneAndConfirmWithACode) {
  using namespace keygrant::detail;
  const PickupAsk ask = pickupAsk(kAddress, std::nullopt);
  EXPECT_EQ(ask.email, kAddress);
  EXPECT_EQ(ask.code, std::nullopt);
  EXPECT_EQ(ask.refused, std::nullopt);
  EXPECT_EQ(pickupAsk(kAddress, "123456").code, "123456");
  EXPECT_EQ(pickupAsk("", "123456").refused, PickupError::InvalidEmail);
}

TEST(PickupPieces, ReadARefusalLeniently) {
  using namespace keygrant::detail;
  EXPECT_EQ(pickupRefusal(json(nullptr)).error, PickupError::Network);
  const PickupRefusal waits = pickupRefusal({{"error", "unavailable"}, {"retryAfter", 60}});
  EXPECT_EQ(waits.error, PickupError::TryAgain);
  EXPECT_EQ(waits.retryAfter, 60);
  EXPECT_EQ(pickupRefusal({{"error", "wrong-code"}, {"retryAfter", 60}}).retryAfter, std::nullopt);
  EXPECT_EQ(pickupRefusal({{"error", "too-many-attempts"}, {"retryAfter", 1.5}}).retryAfter, std::nullopt);
}

TEST(PickupPieces, ReadAConfirmsKeysLenientlyAndNoneFromWhatIsNoAnswer) {
  using namespace keygrant::detail;
  EXPECT_FALSE(pickedKeysOf({{"keys", "no"}}).has_value());
  EXPECT_FALSE(pickedKeysOf(json::array()).has_value());
  EXPECT_FALSE(pickedKeysOf(json(nullptr)).has_value());
  const auto read = pickedKeysOf({{"keys", json::array({{{"key", ""}}, 7, {{"key", KEY}, {"held", "yes"}, {"activationCode", "12345"}}})}});
  ASSERT_TRUE(read.has_value());
  ASSERT_EQ(read->size(), 1u);
  PickedKey expected;
  expected.key = KEY;
  EXPECT_EQ((*read)[0], expected);
}

TEST(PickupPieces, JudgeAKeysVersionsAsTheServerDoes) {
  using namespace keygrant::detail;
  EXPECT_TRUE(covers(std::nullopt, 9, NOW));
  EXPECT_FALSE(covers(PickedVersions{2, 3, std::nullopt}, 1, NOW));
  EXPECT_FALSE(covers(PickedVersions{2, 3, std::nullopt}, 4, NOW));
  EXPECT_TRUE(covers(PickedVersions{2, 3, NOW + 1}, 4, NOW));
  EXPECT_FALSE(covers(PickedVersions{2, 3, NOW}, 4, NOW));
  EXPECT_TRUE(covers(PickedVersions{2, std::nullopt, std::nullopt}, 40, NOW));
}

TEST(PickupPieces, ChooseTheKeyHeldThenOneWithAFreeSeatThenTheFirst) {
  using namespace keygrant::detail;
  auto key = [](const std::string& name, std::int64_t active, bool held, std::optional<PickedVersions> versions) {
    PickedKey k;
    k.key = name;
    k.versions = versions;
    k.deviceLimit = 1;
    k.activeDevices = active;
    k.held = held;
    return k;
  };
  const PickedVersions one{1, 1, std::nullopt};
  const PickedKey full = key("A", 1, false, one);
  const PickedKey free = key("B", 0, false, one);
  const PickedKey held = key("C", 1, true, one);
  EXPECT_EQ(chosenKey({full, free, held}, 1, NOW).index, 2u);
  EXPECT_EQ(chosenKey({full, free}, 1, NOW).index, 1u);
  EXPECT_EQ(chosenKey({full, key("D", 1, false, one)}, 1, NOW).index, 0u);
  EXPECT_EQ(chosenKey({key("E", 0, false, PickedVersions{3, std::nullopt, std::nullopt})}, 1, NOW).refused, Unchosen::Outdated);
  EXPECT_EQ(chosenKey({}, 1, NOW).refused, Unchosen::NoKey);
}

}  // namespace
}  // namespace kgtest
