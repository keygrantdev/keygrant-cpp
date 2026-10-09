// The lease's cryptography and claims, and the schema's edges: canonical base64url, the public key,
// verification, and the entitlements claim read leniently.
#include "crypto.hpp"
#include "lease.hpp"
#include "support.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;

std::string text(const Bytes& bytes) { return std::string(bytes.begin(), bytes.end()); }

TEST(Base64Url, DecodesCanonicalBase64UrlOnly) {
  EXPECT_EQ(text(*base64UrlDecode("aGVsbG8")), "hello");
  EXPECT_EQ(text(*base64UrlDecode("")), "");
  EXPECT_EQ(text(*base64UrlDecode("-_8")), "\xfb\xff");
  // Padding, the standard alphabet, white space and stray bits are not canonical.
  EXPECT_FALSE(base64UrlDecode("aGVsbG8="));
  EXPECT_FALSE(base64UrlDecode("+/8"));
  EXPECT_FALSE(base64UrlDecode("aGVs bG8"));
  EXPECT_FALSE(base64UrlDecode("aGVsbG9"));  // the last character carries a stray bit
  EXPECT_FALSE(base64UrlDecode("a"));        // a length no encoder writes
  EXPECT_FALSE(base64UrlDecode("!!!"));
}

TEST(Base64Url, RoundTripsEveryLength) {
  std::string bytes;
  for (int n = 0; n < 70; ++n) {
    const std::string encoded = base64UrlEncode(bytes);
    EXPECT_EQ(encoded.find_first_of("+/="), std::string::npos);
    const auto decoded = base64UrlDecode(encoded);
    ASSERT_TRUE(decoded) << n;
    EXPECT_EQ(text(*decoded), bytes);
    bytes.push_back(static_cast<char>(n * 37 + 11));
  }
}

TEST(Base64Url, RefusesPaddingTheStandardAlphabetWhiteSpaceAndStrayBits) {
  EXPECT_EQ(text(*base64UrlDecode("-_8")), "\xfb\xff");
  EXPECT_FALSE(base64UrlDecode("aGVsbG8="));
  EXPECT_FALSE(base64UrlDecode("+/8"));
  EXPECT_FALSE(base64UrlDecode("aGVs bG8"));
  EXPECT_FALSE(base64UrlDecode("QR"));
  EXPECT_FALSE(base64UrlDecode("a"));
}

TEST(Sha256, IsLowercaseHex) {
  EXPECT_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(FingerprintClaim, IsAStableHashNeverTheFingerprint) {
  const std::string claim = fingerprintClaim("machine-guid-1234");
  EXPECT_EQ(claim.size(), 64u);
  EXPECT_EQ(claim.find("machine-guid"), std::string::npos);
  EXPECT_EQ(fingerprintClaim("machine-guid-1234"), claim);
  EXPECT_NE(fingerprintClaim("machine-guid-5678"), claim);
  EXPECT_EQ(claim, sha256Hex("keygrant-fp|machine-guid-1234"));
}

TEST(PublicKey, ReadsTheJwkOrItsBareX) {
  const TestKey key;
  const std::string jwk = key.publicJwk();
  const std::string x = json::parse(jwk)["x"].get<std::string>();
  const std::string token = key.sign(claimsOf(LeaseSpec{}));
  const std::vector<std::string> forms = {
      jwk,
      x,
      "  " + jwk + "\n",
      R"({"kty":"OKP","crv":"Ed25519","alg":"EdDSA","use":"sig","key_ops":["verify"],"x":")" + x + "\"}",
      x + "=",
  };
  for (const std::string& given : forms) {
    EXPECT_TRUE(verifyLease(token, KeySet::fromJwk(given), NOW).valid) << given;
  }
}

TEST(PublicKey, RefusesWhatIsNotAnEd25519PublicKey) {
  const TestKey key;
  const std::string x = json::parse(key.publicJwk())["x"].get<std::string>();
  const std::vector<std::string> bad = {
      "",
      "   ",
      "{not json",
      "[]",
      R"({"kty":"EC","crv":"Ed25519","x":")" + x + "\"}",
      R"({"kty":"OKP","crv":"X25519","x":")" + x + "\"}",
      R"({"kty":"OKP","crv":"Ed25519"})",
      R"({"kty":"OKP","crv":"Ed25519","x":42})",
      // A private key embedded in an app is a leak: refused, by name.
      R"({"kty":"OKP","crv":"Ed25519","x":")" + x + R"(","d":"secret"})",
      R"({"kty":"OKP","crv":"Ed25519","alg":"RS256","x":")" + x + "\"}",
      R"({"kty":"OKP","crv":"Ed25519","use":"enc","x":")" + x + "\"}",
      R"({"kty":"OKP","crv":"Ed25519","key_ops":["sign"],"x":")" + x + "\"}",
      x.substr(0, 20),
      x + x,
  };
  for (const std::string& given : bad) EXPECT_THROW(VerifyingKey::fromJwk(given), std::invalid_argument) << given;
}

class Verify : public ::testing::Test {
 protected:
  TestKey key;
  KeySet verifying = KeySet::fromJwk(key.publicJwk());

  VerifyResult verifyClaims(const ordered_json& claims, std::int64_t now = NOW) {
    return verifyLease(key.sign(claims), verifying, now);
  }
  VerifyResult verifyWith(const std::string& name, const ordered_json& value) {
    ordered_json claims = claimsOf(LeaseSpec{});
    claims[name] = value;
    return verifyClaims(claims);
  }
  static ordered_json without(ordered_json claims, const std::string& name) {
    claims.erase(name);
    return claims;
  }
  /** A lease whose `ent` is this exact JSON text, as the server's bytes carry it (not as nlohmann would write it). */
  VerifyResult verifyEntText(const std::string& ent) {
    const std::string payload = R"({"iss":"keygrant","sub":")" + std::string(KEY) +
                                R"(","product":"sluice","aid":"act_1","model":"perpetual","lim":3,"major":1,"ent":)" +
                                ent + R"(,"iat":1756913600,"exp":1759505600})";
    return verifyLease(key.signRaw(payload), verifying, NOW);
  }
};

TEST_F(Verify, AValidLeaseAndItsClaims) {
  LeaseSpec spec;
  spec.extra = {{"fp", claimOf("fp")}, {"fpk", "machine"}, {"minMajor", 2}, {"upgradeUntil", 1'800'000'000},
                {"grace", true}, {"test", true}, {"ent", {{"pro", true}, {"seats", 5}, {"tier", "gold"}}}};
  const VerifyResult v = verifyClaims(claimsOf(spec));
  ASSERT_TRUE(v.valid);
  const LeaseClaims& c = *v.claims;
  EXPECT_EQ(c.sub, KEY);
  EXPECT_EQ(c.fp, claimOf("fp"));
  EXPECT_EQ(c.fpk, DeviceKind::Machine);
  EXPECT_EQ(c.minMajor, 2);
  EXPECT_EQ(c.upgradeUntil, 1'800'000'000);
  EXPECT_EQ(c.grace, true);
  EXPECT_EQ(c.test, true);
  EXPECT_EQ(c.ent, (std::map<std::string, keygrant::Entitlement>{{"pro", true}, {"seats", 5}, {"tier", "gold"}}));
  EXPECT_EQ(c.exp, c.iat + 30 * 24 * 3600);
}

TEST_F(Verify, RefusesAnotherKeysSignatureAndAChangedPayload) {
  const TestKey other;
  const std::string token = other.sign(claimsOf(LeaseSpec{}));
  EXPECT_EQ(verifyLease(token, verifying, NOW).reason, VerifyFailure::BadSignature);
  const std::string mine = key.sign(claimsOf(LeaseSpec{}));
  LeaseSpec forged;
  forged.major = keygrant::kOpenMajor;
  const std::string payload = base64UrlEncode(claimsOf(forged).dump());
  const std::size_t first = mine.find('.');
  const std::size_t second = mine.find('.', first + 1);
  const std::string tampered = mine.substr(0, first + 1) + payload + mine.substr(second);
  EXPECT_EQ(verifyLease(tampered, verifying, NOW).reason, VerifyFailure::BadSignature);
}

TEST_F(Verify, IsMalformedWithoutThreePartsOrADecodableSignature) {
  const std::string token = key.sign(claimsOf(LeaseSpec{}));
  EXPECT_EQ(verifyLease("only.two", verifying, NOW).reason, VerifyFailure::Malformed);
  EXPECT_EQ(verifyLease(token + ".four", verifying, NOW).reason, VerifyFailure::Malformed);
  EXPECT_EQ(verifyLease("", verifying, NOW).reason, VerifyFailure::Malformed);
  EXPECT_EQ(verifyLease(token + "=", verifying, NOW).reason, VerifyFailure::Malformed);
  // An empty signature decodes (to nothing), and nothing is no signature of this key.
  EXPECT_EQ(verifyLease(token.substr(0, token.rfind('.') + 1), verifying, NOW).reason, VerifyFailure::BadSignature);
}

TEST_F(Verify, ExpiresOnlyAfterItsLastMillisecond) {
  LeaseSpec spec;
  spec.ttlSeconds = 60;
  const std::string token = key.sign(claimsOf(spec));
  const std::int64_t end = (NOW / 1000 + 60) * 1000;
  EXPECT_TRUE(verifyLease(token, verifying, end).valid);
  const VerifyResult past = verifyLease(token, verifying, end + 1);
  EXPECT_EQ(past.reason, VerifyFailure::Expired);
  ASSERT_TRUE(past.claims);  // still given
  EXPECT_EQ(past.claims->sub, KEY);
}

TEST_F(Verify, ReadsTheSchemasEdgesAsTheReferenceDoes) {
  const ordered_json base = claimsOf(LeaseSpec{});
  // An optional claim that is present but null is not absent: the schema refuses it (`ent` aside,
  // which is read leniently: below).
  for (const char* name : {"fp", "fpk", "minMajor", "upgradeUntil", "grace", "test", "major"}) {
    EXPECT_EQ(verifyWith(name, nullptr).reason, VerifyFailure::BadClaims) << name;
  }
  // Absent: a default (major 0, ent {}), or simply none.
  const VerifyResult defaults = verifyClaims(without(without(base, "major"), "ent"));
  ASSERT_TRUE(defaults.valid);
  EXPECT_EQ(defaults.claims->major, 0);
  EXPECT_TRUE(defaults.claims->ent.empty());
  // A whole number written as a float is a whole number (Number.isInteger).
  ordered_json floats = base;
  floats["exp"] = static_cast<double>(base["exp"].get<std::int64_t>());
  floats["lim"] = 3.0;
  EXPECT_TRUE(verifyClaims(floats).valid);
  EXPECT_EQ(verifyWith("lim", 2.5).reason, VerifyFailure::BadClaims);
  EXPECT_EQ(verifyWith("iat", -1).reason, VerifyFailure::BadClaims);
  EXPECT_EQ(verifyWith("minMajor", 0).reason, VerifyFailure::BadClaims);
  EXPECT_EQ(verifyWith("fpk", "MACHINE").reason, VerifyFailure::BadClaims);
  EXPECT_EQ(verifyWith("model", "lifetime").reason, VerifyFailure::BadClaims);
  EXPECT_EQ(verifyWith("iss", "someone").reason, VerifyFailure::BadClaims);
  EXPECT_TRUE(verifyWith("somethingNew", {{"x", 1}}).valid);
}

using Entitlements = std::map<std::string, keygrant::Entitlement>;

TEST_F(Verify, KeepsEveryKindOfEntitlementAsSigned) {
  const ordered_json ent = {{"edition", "pro"}, {"seats", 5},   {"ratio", 0.5},
                            {"discount", -2},   {"beta", false}, {"label", ""}};
  const VerifyResult v = verifyWith("ent", ent);
  ASSERT_TRUE(v.valid);
  EXPECT_EQ(v.claims->ent, (Entitlements{{"edition", "pro"}, {"seats", 5}, {"ratio", 0.5}, {"discount", -2},
                                         {"beta", false}, {"label", ""}}));
  // A number is a double, whichever way JSON wrote it.
  EXPECT_EQ(v.claims->ent.at("seats").number(), 5.0);
  EXPECT_EQ(claimsJson(*v.claims)["ent"], json::parse(ent.dump()));
}

TEST_F(Verify, KeepsTheGoodMembersOfAMalformedEntAndDropsTheRestTheLeaseStillValid) {
  // A signed lease is the server's word: one bad member must not take the
  // licence away, nor the good members beside it.
  const ordered_json ent = {{"nested", {{"tier", "gold"}}}, {"edition", "pro"}, {"none", nullptr},
                            {"list", {"a"}},                 {"seats", 2}};
  const VerifyResult v = verifyWith("ent", ent);
  ASSERT_TRUE(v.valid);
  EXPECT_EQ(v.claims->ent, (Entitlements{{"edition", "pro"}, {"seats", 2}}));
}

TEST_F(Verify, ReadsAnEntThatIsNotAnObjectAsNoneNeverAsARefusal) {
  for (const ordered_json& ent : {ordered_json(nullptr), ordered_json::array(), ordered_json::array({"pro", true}),
                                  ordered_json("pro"), ordered_json(42), ordered_json(true)}) {
    const VerifyResult v = verifyWith("ent", ent);
    ASSERT_TRUE(v.valid) << ent.dump();
    EXPECT_TRUE(v.claims->ent.empty()) << ent.dump();
  }
}

TEST_F(Verify, TakesEntitlementNamesAsTheyAreProtoAndBeyondAsciiIncluded) {
  const std::string payload = R"({"iss":"keygrant","sub":")" + std::string(KEY) +
                              R"(","product":"sluice","aid":"act_1","model":"perpetual","lim":3,"major":1,)"
                              R"("ent":{"__proto__":"pro","seats":1,"édition":"Pro Studio ✓"},)"
                              R"("iat":1756913600,"exp":1759505600})";
  const VerifyResult v = verifyLease(key.signRaw(payload), verifying, NOW);
  ASSERT_TRUE(v.valid);
  EXPECT_EQ(v.claims->ent,
            (Entitlements{{"__proto__", "pro"}, {"seats", 1}, {"\xC3\xA9" "dition", "Pro Studio \xE2\x9C\x93"}}));
}

TEST_F(Verify, ReadsEntitlementNumbersAsTheNearestDoubleAsJavaScriptDoes) {
  // Only a correctly rounded reader gives these exactly; the compiler's own
  // literals are the nearest doubles to compare them with.
  const VerifyResult v = verifyEntText(
      R"({"ratio":221.62324973788606,"share":975.1952520418383,"total":123456789012345680000,"huge":1e21})");
  ASSERT_TRUE(v.valid);
  EXPECT_EQ(v.claims->ent.at("ratio").number(), 221.62324973788606);
  EXPECT_EQ(v.claims->ent.at("share").number(), 975.1952520418383);
  EXPECT_EQ(v.claims->ent.at("total").number(), 123456789012345680000.0);
  EXPECT_EQ(v.claims->ent.at("huge").number(), 1e21);
}

TEST_F(Verify, RefusesWholeAPayloadItsJsonReaderCannotReadALoneSurrogateOrANumberPastADouble) {
  // As the README's parity list says: nlohmann/json refuses an escaped lone surrogate and a number
  // past a double's range, so the lease is bad-claims here, where the reference keeps the
  // entitlement. The signer writes neither. \x5c is a backslash: a JSON escape in the payload,
  // never a C++ one.
  EXPECT_EQ(verifyEntText("{\"edition\":\"\x5cud800\"}").reason, VerifyFailure::BadClaims);
  EXPECT_EQ(verifyEntText("{\"\x5cudc00edition\":\"pro\"}").reason, VerifyFailure::BadClaims);
  EXPECT_EQ(verifyEntText(R"({"seats":1e400})").reason, VerifyFailure::BadClaims);
  // The same payloads with whole characters and a double's number read as usual.
  EXPECT_TRUE(verifyEntText("{\"edition\":\"\x5cud83c\x5cudfb5\",\"seats\":1e300}").valid);
}

TEST_F(Verify, TakesEntitlementNamesAndTextsPastUffffWholeWrittenRawOrEscaped) {
  // U+1F3B5 and U+1D11E: four UTF-8 bytes each, or a JSON surrogate pair each.
  const Entitlements expected{{"\xF0\x9F\x8E\xB5", "Pro \xF0\x9D\x84\x9E"}};
  const std::string raw = "{\"\xF0\x9F\x8E\xB5\":\"Pro \xF0\x9D\x84\x9E\"}";
  const std::string escaped = "{\"\x5cud83c\x5cudfb5\":\"Pro \x5cud834\x5cudd1e\"}";
  for (const std::string& ent : {raw, escaped}) {
    const VerifyResult v = verifyEntText(ent);
    ASSERT_TRUE(v.valid) << ent;
    EXPECT_EQ(v.claims->ent, expected) << ent;
  }
}

TEST_F(Verify, EnforcesNoneOfTheCapsTheServerHoldsEntitlementsTo) {
  // The signature already says the server stood behind them.
  ordered_json ent = ordered_json::object();
  Entitlements expected;
  for (int i = 0; i < 20; ++i) {
    ent["n" + std::to_string(i)] = i;
    expected.emplace("n" + std::to_string(i), i);
  }
  ent[std::string(40, 'k')] = true;
  expected.emplace(std::string(40, 'k'), true);
  ent["long"] = std::string(100, 'v');
  expected.emplace("long", std::string(100, 'v'));
  const VerifyResult v = verifyWith("ent", ent);
  ASSERT_TRUE(v.valid);
  EXPECT_EQ(v.claims->ent, expected);
}

TEST(EntitlementsOf, ReadsTheParsedValueTheSameWay) {
  EXPECT_EQ(entitlementsOf(json::parse(R"({"a":true,"b":1,"c":"x","d":{},"e":[],"f":null})")),
            (Entitlements{{"a", true}, {"b", 1}, {"c", "x"}}));
  EXPECT_TRUE(entitlementsOf(json()).empty());
  EXPECT_TRUE(entitlementsOf(json::parse(R"([["a",true]])")).empty());
}

TEST_F(Verify, CountsADeviceClaimsLengthInUtf16Units) {
  // 64 emoji are 128 UTF-16 units: the most a claim may be. 65 are 130.
  std::string emoji;
  for (int i = 0; i < 64; ++i) emoji += "\xF0\x9F\x98\x80";
  EXPECT_TRUE(verifyWith("fp", emoji).valid);
  EXPECT_EQ(verifyWith("fp", emoji + "\xF0\x9F\x98\x80").reason, VerifyFailure::BadClaims);
  EXPECT_EQ(verifyWith("fp", "").reason, VerifyFailure::BadClaims);
}

TEST_F(Verify, HoldsAFarFutureLeaseValidAsJavaScriptDoes) {
  // Beyond 2^53 a number is saturated, not refused: still in the far future.
  ordered_json claims = claimsOf(LeaseSpec{});
  claims["exp"] = 1e20;
  const VerifyResult v = verifyClaims(claims);
  ASSERT_TRUE(v.valid);
  EXPECT_GT(v.claims->exp, NOW);
}

TEST_F(Verify, ReadsAPayloadOfIllFormedUtf8AsTextDecoderDoes) {
  // Bytes that are not UTF-8 inside a string become U+FFFD; the claims still parse.
  const std::string payload = R"({"iss":"keygrant","sub":"K)" + std::string("\xff") +
                              R"(","product":"sluice","aid":"act_1","model":"perpetual","lim":3,"major":1,"ent":{},"iat":1756913600,"exp":1759505600})";
  const VerifyResult v = verifyLease(key.signRaw(payload), verifying, NOW);
  ASSERT_TRUE(v.valid);
  EXPECT_EQ(v.claims->sub, "K\xEF\xBF\xBD");
}

}  // namespace
}  // namespace kgtest
