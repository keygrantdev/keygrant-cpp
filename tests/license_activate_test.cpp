// activate(), deactivate(), upgradeUrl() and test licences (sdk/src/license.test.ts).
#include "support.hpp"

namespace kgtest {
namespace {

json leaseAnswer(const std::string& lease, const std::string& activationId = "act_1") {
  return {{"lease", lease}, {"activationId", activationId}};
}

TEST(Activate, CutsADeviceNameTheServerWouldRefuseWithoutSplittingACharacter) {
  // The name is a label in a device list, sent to a public endpoint that bounds it.
  const std::string emoji = "\xF0\x9F\x98\x80";
  const std::vector<std::pair<std::string, std::string>> names = {
      {std::string(500, 'D'), std::string(120, 'D')},
      {std::string(119, 'D') + emoji, std::string(119, 'D')},
      {std::string(120, 'D'), std::string(120, 'D')},
      {"Rae\xE2\x80\x99s MacBook", "Rae\xE2\x80\x99s MacBook"},
  };
  for (const auto& [given, sent] : names) {
    HarnessOptions options;
    options.deviceName = given;
    Harness h(std::nullopt, options);
    h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
    EXPECT_EQ(h.license().activate(KEY), ok());
    EXPECT_EQ(h.http->bodyOf("activate")["name"], sent);
  }
}

TEST(Activate, SendsNoNameWhenTheAppGivesNone) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  h.license().activate(KEY);
  EXPECT_FALSE(h.http->bodyOf("activate").contains("name"));
  EXPECT_EQ(h.http->bodyOf("activate"), json({{"key", KEY}, {"fingerprint", "fp-test"}, {"major", 1}}));
}

TEST(Activate, RefusesAKeyThatCannotBeOneWithoutAskingAnybody) {
  Harness h;
  EXPECT_EQ(h.license().activate(std::string(200, 'S')), failed(ActivateError::InvalidKey));
  EXPECT_EQ(h.license().activate("   "), failed(ActivateError::InvalidKey));
  EXPECT_EQ(h.license().activate(""), failed(ActivateError::InvalidKey));
  EXPECT_FALSE(h.http->called("activate"));
}

TEST(Activate, SendsAKeyOfExactlyTheLimitAndDoesNotCountTheWhitespaceAroundIt) {
  const std::string key = "SLUICE-" + std::string(113, 'A');
  ASSERT_EQ(key.size(), 120u);
  for (const std::string& entered : {key, key + "\n"}) {
    Harness h;
    h.http->reply("activate", 200, leaseAnswer(h.sign(key, "perpetual", 30 * 24 * 3600)));
    EXPECT_EQ(h.license().activate(entered), ok());
    EXPECT_EQ(h.http->bodyOf("activate")["key"], key);
  }
}

TEST(Activate, NormalisesAPastedOrTypedKeyAndStoresTheNormalisedOne) {
  // A paste carries a trailing newline more often than not; a person types in any case.
  for (const char* entered : {"  SLUICE-AAAA-BBBB-CCCC-DDDD\n", "sluice-aaaa-bbbb-cccc-dddd"}) {
    Harness h;
    h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
    EXPECT_EQ(h.license().activate(entered), ok());
    EXPECT_EQ(h.http->bodyOf("activate")["key"], KEY);
    // The stored key is the normalised one, so the deactivate after it names a key the server finds.
    EXPECT_EQ(h.stored()->key, KEY);
  }
}

TEST(Activate, StoresAVerifiedLeaseAndReportsLicensed) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.stored()->key, KEY);
  EXPECT_EQ(h.stored()->activationId, "act_1");
  EXPECT_EQ(h.stored()->checkedMajor, 1);
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.http->count("validate"), 0);
}

TEST(Activate, MapsTheServersErrors) {
  const std::vector<std::tuple<int, const char*, ActivateError>> cases = {
      {404, "invalid-key", ActivateError::InvalidKey}, {409, "limit", ActivateError::Limit},
      {403, "revoked", ActivateError::Revoked},        {403, "expired", ActivateError::Revoked},
      {403, "upgrade-required", ActivateError::Upgrade}, {403, "update-required", ActivateError::Outdated},
      {400, "weird", ActivateError::Network},           {500, "boom", ActivateError::Network},
  };
  for (const auto& [code, word, error] : cases) {
    Harness h;
    h.http->reply("activate", code, {{"error", word}});
    EXPECT_EQ(h.license().activate("KEY"), failed(error)) << word;
    EXPECT_EQ(h.stored(), std::nullopt) << word;
  }
}

TEST(Activate, IsNetworkWithNoAnswerOrA200WithoutBothALeaseAndAnActivation) {
  Harness down;
  down.http->fail = true;
  EXPECT_EQ(down.license().activate("KEY"), failed(ActivateError::Network));
  for (const json& body : {json{{"activationId", "act_1"}}, json{{"lease", "a.b.c"}}, json{{"lease", ""}, {"activationId", "act_1"}},
                           json{{"lease", "a.b.c"}, {"activationId", ""}}, json("not an object")}) {
    Harness h;
    h.http->reply("activate", 200, body);
    EXPECT_EQ(h.license().activate("KEY"), failed(ActivateError::Network)) << body;
  }
  Harness html;
  html.http->on("activate", [](const json&) { return keygrant::HttpResponse{200, "<html>portal</html>"}; });
  EXPECT_EQ(html.license().activate("KEY"), failed(ActivateError::Network));
}

TEST(Activate, IsInvalidLeaseWhenTheServersLeaseFailsOfflineVerification) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer("not.a.jwt"));
  EXPECT_EQ(h.license().activate("KEY"), failed(ActivateError::InvalidLease));
  // One for another product, or a build it does not cover, is not this licence either.
  Harness other;
  LeaseSpec spec;
  spec.product = "maindeck";
  other.http->reply("activate", 200, leaseAnswer(other.sign(spec)));
  EXPECT_EQ(other.license().activate(KEY), failed(ActivateError::InvalidLease));
  EXPECT_EQ(other.stored(), std::nullopt);
}

TEST(Activate, FailsWhenTheStateCannotBeSavedTheCustomerCanTryAgain) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  h.storage->failWrites = true;
  EXPECT_THROW(h.license().activate(KEY), keygrant::IoError);
}

TEST(Deactivate, ReleasesTheActivationAndClearsTheLocalLicence) {
  Harness h;
  h.store(held(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  h.http->reply("deactivate", 200, {{"ok", true}});
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
  EXPECT_EQ(h.http->bodyOf("deactivate"), json({{"key", KEY}, {"activationId", "act_1"}}));
  EXPECT_EQ(h.stored()->key, std::nullopt);
  EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
}

TEST(Deactivate, IsNotReleasedWhenTheServerIsUnreachableTheLocalLicenceClearedAllTheSame) {
  Harness h;
  h.store(held(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  h.http->fail = true;
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{false});
  EXPECT_TRUE(h.http->called("deactivate"));
  EXPECT_EQ(h.stored()->key, std::nullopt);
  EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
}

TEST(Deactivate, IsNotReleasedWhenTheServerDoesNotAnswerInTime) {
  HarnessOptions options;
  options.httpTimeout = Millis{50};
  Harness h(std::nullopt, options);
  h.store(held(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  h.http->hang = true;
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{false});
  EXPECT_EQ(h.stored()->key, std::nullopt);
  h.http->hangGate().open();
}

TEST(Deactivate, IsNotReleasedOnAnyAnswerButA200TheLocalLicenceClearedAllTheSame) {
  // A 5xx, a 429, a proxy's page, a refusal: answers, but not KeyGrant saying the seat is free.
  const std::vector<keygrant::HttpResponse> answers = {
      {500, R"({"error":"internal"})"},
      {503, ""},
      {429, R"({"error":"rate-limited"})"},
      {502, "<html><body>Bad Gateway</body></html>"},
      {404, R"({"error":"unknown-product"})"},
      {403, R"({"error":"revoked"})"},
      {204, ""},
  };
  for (const keygrant::HttpResponse& answer : answers) {
    SCOPED_TRACE(answer.status);
    Harness h;
    h.store(held(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
    h.http->on("deactivate", [answer](const json&) { return answer; });
    EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{false});
    EXPECT_EQ(h.stored()->key, std::nullopt);
    EXPECT_EQ(h.stored()->lease, std::nullopt);
  }
}

TEST(Deactivate, IsReleasedWithNoActivationToReleaseAskingNothing) {
  // Nothing stored, a key without an activation, an activation without a key.
  StoredState keyOnly;
  keyOnly.key = KEY;
  StoredState activationOnly;
  activationOnly.activationId = "act_1";
  StoredState empty;
  empty.key = "";
  empty.activationId = "";
  for (const std::optional<StoredState>& state :
       std::vector<std::optional<StoredState>>{std::nullopt, keyOnly, activationOnly, empty}) {
    Harness h(state);
    EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
    EXPECT_FALSE(h.http->called("deactivate"));
    EXPECT_EQ(h.stored()->key, std::nullopt);
    EXPECT_EQ(h.stored()->activationId, std::nullopt);
    EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
  }
}

TEST(Deactivate, KeepsOnlyTheTrialEvidenceAndTheClockGuard) {
  Harness h;
  StoredState state = held(h.sign(KEY, "perpetual", 30 * 24 * 3600));
  state.trialStart = NOW - 3 * DAY;
  state.trialEndsAt = NOW + 4 * DAY;
  state.trialAt = NOW - 3 * DAY;
  state.keptMajor = 2;
  state.refused = keygrant::Refusal::Device;
  state.refusedFor = std::vector<std::string>{"c"};
  state.backoffSince = NOW;
  state.backoffMs = 60'000;
  state.deviceKind = DeviceKind::Machine;
  h.store(state);
  h.license().deactivate();
  StoredState expected;
  expected.trialStart = NOW - 3 * DAY;
  expected.trialEndsAt = NOW + 4 * DAY;
  expected.trialAt = NOW - 3 * DAY;
  expected.lastSeen = NOW;
  expected.verdictAt = NOW;
  EXPECT_EQ(h.stored(), expected);
}

TEST(Deactivate, FailsWhenTheStateCannotBeSavedTheKeyKeptToTryAgain) {
  Harness h;
  const std::string lease = h.sign(KEY, "perpetual", 30 * 24 * 3600);
  h.store(held(lease));
  h.http->reply("deactivate", 200, {{"ok", true}});
  h.storage->failWrites = true;
  EXPECT_THROW(h.license().deactivate(), keygrant::IoError);
  EXPECT_EQ(h.stored()->key, KEY);
  EXPECT_EQ(h.stored()->lease, lease);
}

// --- deactivate, when the activation stored is another device's -------------------------------
// A profile copied or restored from another PC carries that PC's activation:
// releasing it would lock that PC out at its next check-in.

/** A 30-day licence lease from `signer`, its seat keyed (`kind`) by the fingerprint `device`, or held to no device. */
std::string leaseHeldTo(const Harness& signer, const std::optional<std::string>& device, const char* kind = "machine",
                        std::int64_t signedAt = NOW) {
  LeaseSpec spec;
  spec.now = signedAt;
  if (device) spec.extra = {{"fp", claimOf(*device)}, {"fpk", kind}};
  return signer.sign(spec);
}

TEST(DeactivateElsewhere, DropsItHereWithoutReleasingThatDevicesSeat) {
  Harness h;
  h.store(held(leaseHeldTo(h, "fp-other-pc")));
  h.http->reply("deactivate", 200, {{"ok", true}});
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
  EXPECT_FALSE(h.http->called("deactivate"));
  EXPECT_EQ(h.stored()->key, std::nullopt);
  EXPECT_EQ(h.stored()->activationId, std::nullopt);
  EXPECT_EQ(h.stored()->lease, std::nullopt);
  EXPECT_EQ(h.stored()->verdictAt, NOW);
}

TEST(DeactivateElsewhere, ReadsWhoseSeatItIsFromALeaseThatHasRunOutToo) {
  Harness h;
  h.store(held(leaseHeldTo(h, "fp-other-pc", "machine", NOW - 40 * DAY)));
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
  EXPECT_FALSE(h.http->called("deactivate"));
  EXPECT_EQ(h.stored()->key, std::nullopt);
}

TEST(DeactivateElsewhere, TakesADeviceRefusalThatStandsForThisDeviceAsTheSame) {
  Harness h;
  StoredState state = held(leaseHeldTo(h, std::nullopt));
  state.refused = keygrant::Refusal::Device;
  state.refusedFor = std::vector<std::string>{claimOf("fp-test")};
  h.store(state);
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
  EXPECT_FALSE(h.http->called("deactivate"));
  EXPECT_EQ(h.stored()->key, std::nullopt);
}

TEST(DeactivateElsewhere, StillReleasesThisDevicesOwnSeatAndOneHeldToNoMachine) {
  struct Case {
    const char* name;
    std::optional<std::string> device;
    const char* kind;
  };
  for (const Case& c : std::vector<Case>{{"this device's", std::string("fp-test"), "machine"},
                                         {"held to no device", std::nullopt, "machine"},
                                         // A host name is never held: a renamed machine keeps its seat.
                                         {"another host name's", std::string("fp-other-pc"), "hostname"}}) {
    Harness h;
    h.store(held(leaseHeldTo(h, c.device, c.kind)));
    h.http->reply("deactivate", 200, {{"ok", true}});
    EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true}) << c.name;
    EXPECT_TRUE(h.http->called("deactivate")) << c.name;
  }
}

TEST(DeactivateElsewhere, ReleasesTheSeatWhenThisRunCannotTellOrTheLeaseIsNotTheProducts) {
  // Not known to be another device's: released as ever.
  auto failing = std::make_shared<FakeFingerprinter>();
  failing->failing = true;
  HarnessOptions options;
  options.fingerprint = failing;
  Harness cannotTell(std::nullopt, options);
  cannotTell.store(held(leaseHeldTo(cannotTell, "fp-other-pc")));
  cannotTell.http->reply("deactivate", 200, {{"ok", true}});
  EXPECT_EQ(cannotTell.license().deactivate(), keygrant::DeactivateResult{true});
  EXPECT_TRUE(cannotTell.http->called("deactivate"));

  // A lease another key signed says nothing of whose seat it is.
  Harness h;
  h.store(held(leaseHeldTo(Harness(), "fp-other-pc")));
  h.http->reply("deactivate", 200, {{"ok", true}});
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
  EXPECT_TRUE(h.http->called("deactivate"));
}

TEST(UpgradeUrl, NamesThisDevicesActivationNeverTheKey) {
  StoredState state;
  state.key = KEY;
  state.activationId = "act_device1";
  Harness h(state);
  EXPECT_EQ(h.license().upgradeUrl("https://buy.stripe.com/test_abc"),
            "https://buy.stripe.com/test_abc?client_reference_id=act_device1");
  const auto url = h.license().upgradeUrl("https://buy.stripe.com/test_abc?client_reference_id=x");
  ASSERT_TRUE(url);
  EXPECT_EQ(url->find("SLUICE"), std::string::npos);
  EXPECT_EQ(*url, "https://buy.stripe.com/test_abc?client_reference_id=act_device1");
}

TEST(UpgradeUrl, IsNoneOnADeviceHoldingNoActivatedLicence) {
  StoredState trial;
  trial.trialStart = NOW;
  StoredState keyOnly;
  keyOnly.key = "KEY-1";
  // An activation without its key: no licence to upgrade, though the id is there.
  StoredState activationOnly;
  activationOnly.activationId = "act_1";
  StoredState emptyKey;
  emptyKey.key = "";
  emptyKey.activationId = "act_1";
  for (const StoredState& state : {trial, keyOnly, activationOnly, emptyKey, StoredState{}}) {
    Harness h(state);
    EXPECT_EQ(h.license().upgradeUrl("https://buy.stripe.com/test_abc"), std::nullopt);
    // Even for a link that is no URL: there is nothing to put on it.
    EXPECT_EQ(h.license().upgradeUrl("not a url"), std::nullopt);
    EXPECT_TRUE(h.http->actions().empty());
  }
}

TEST(UpgradeUrl, ThrowsForALinkThatIsNotAUrlAndWhenTheStateCannotBeRead) {
  StoredState state;
  state.key = KEY;
  state.activationId = "act_1";
  Harness h(state);
  EXPECT_THROW(h.license().upgradeUrl("not a url"), std::invalid_argument);
  h.storage->failReads = true;
  EXPECT_THROW(h.license().upgradeUrl("https://buy.stripe.com/x"), keygrant::IoError);
}

std::string testLease(const Harness& h, std::int64_t ttlSeconds = 30 * 24 * 3600, std::int64_t now = NOW) {
  LeaseSpec spec;
  spec.sub = "SLUICE-TEST-TEST-TEST-TEST";
  spec.ttlSeconds = ttlSeconds;
  spec.now = now;
  spec.extra = {{"test", true}};
  return h.sign(spec);
}

TEST(TestLicences, LicenseTheAppAndSaySoSoAReleaseBuildCanRefuseThem) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(testLease(h)));
  EXPECT_EQ(h.license().activate("SLUICE-TEST-TEST-TEST-TEST"), ok());
  EXPECT_EQ(h.license().status(), licensed("SLUICE-TEST-TEST-TEST-TEST", true));
}

TEST(TestLicences, SayNothingOfItOnARealLicence) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  h.license().activate(KEY);
  EXPECT_FALSE(h.license().status().test);
}

TEST(TestLicences, KeepSayingSoAfterARenewalAndWhenARefreshCannotReachTheServer) {
  // From an EXPIRED lease, so the status can only come from the renewed one.
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(testLease(h, 60)));
  h.license().activate("SLUICE-TEST-TEST-TEST-TEST");
  const std::int64_t later = NOW + 2 * 3'600'000;
  h.setClock(later);
  h.http->reply("validate", 200, {{"lease", testLease(h, 30 * 24 * 3600, later)}});
  EXPECT_EQ(h.license().status(), licensed("SLUICE-TEST-TEST-TEST-TEST", true));
  EXPECT_TRUE(h.http->called("validate"));
  h.http->fail = true;
  EXPECT_EQ(h.license().refresh(), licensed("SLUICE-TEST-TEST-TEST-TEST", true));
  EXPECT_EQ(h.http->actions(), (std::vector<std::string>{"activate", "validate", "validate"}));
}

TEST(Config, RefusesABadConfigAtConstruction) {
  const Harness h;
  const auto refused = [&](const std::function<void(keygrant::LicenseConfig&)>& change) {
    keygrant::LicenseConfig config = h.config;
    change(config);
    EXPECT_THROW(keygrant::License{config}, std::invalid_argument);
  };
  refused([](auto& c) { c.publicJwk = "{not json"; });
  refused([](auto& c) { c.publicJwk = R"({"kty":"OKP","crv":"Ed25519","x":"short"})"; });
  refused([](auto& c) { c.product = ""; });
  refused([](auto& c) { c.product = "../escape"; });
  refused([](auto& c) { c.product = "has space"; });
  refused([](auto& c) { c.product = std::string(65, 'p'); });
  refused([](auto& c) { c.apiBaseUrl = ""; });
  refused([](auto& c) { c.apiBaseUrl = "api.keygrant.dev"; });
  refused([](auto& c) { c.apiBaseUrl = "ftp://api.keygrant.dev"; });
  refused([](auto& c) { c.apiBaseUrl = "https://"; });
  refused([](auto& c) { c.major = 0; });
  refused([](auto& c) { c.major = keygrant::kMaxMajor + 1; });
  refused([](auto& c) { c.httpTimeout = Millis{0}; });
  // And takes a good one, the bare `x` included.
  keygrant::LicenseConfig bare = h.config;
  bare.publicJwk = json::parse(h.key.publicJwk())["x"].get<std::string>();
  bare.apiBaseUrl = "HTTPS://api.keygrant.dev";
  bare.major = keygrant::kMaxMajor;
  EXPECT_NO_THROW(keygrant::License{bare});
}

TEST(Config, SendsToTheApiBaseWithoutItsTrailingSlash) {
  const Harness h;  // its base is "https://api.test/"
  keygrant::LicenseConfig config = h.config;
  struct Url final : keygrant::HttpAdapter {
    std::string seen;
    keygrant::HttpResponse post(const std::string& u, const std::string&, Millis) override {
      seen = u;
      return {500, "{}"};
    }
  };
  auto adapter = std::make_shared<Url>();
  config.adapters.http = adapter;
  keygrant::License(config).activate(KEY);
  EXPECT_EQ(adapter->seen, "https://api.test/v1/products/sluice/activate");
}

}  // namespace
}  // namespace kgtest
