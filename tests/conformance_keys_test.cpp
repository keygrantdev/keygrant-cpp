// The key-set groups of conformance/vectors.json: a key's id, a lease verified against a key set,
// and whether entitlements grant a code, each answered from its own JSON.
#include <string>
#include <vector>

#include "conformance.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;

TEST(Conformance, KeyIdsAreTheThumbprintsTheReferenceComputes) {
  for (const json& v : group("keyIds")) {
    SCOPED_TRACE(nameOf(v));
    // null for a JWK the SDK refuses.
    if (v["expect"].is_null()) {
      EXPECT_THROW(VerifyingKey::fromJwk(v["jwk"].dump()), std::invalid_argument);
    } else {
      EXPECT_EQ(VerifyingKey::fromJwk(v["jwk"].dump()).kid(), v["expect"].get<std::string>());
    }
  }
}

TEST(Conformance, KeySetsVerifyAsTheReference) {
  for (const json& v : group("keySets")) {
    SCOPED_TRACE(nameOf(v));
    const KeySet keys = KeySet::fromJwk(v["keys"].dump());
    EXPECT_EQ(json(keys.kids()), v["kids"]);
    const VerifyResult r = verifyLease(v["token"].get<std::string>(), keys, v["now"].get<std::int64_t>());
    json verified = {{"valid", r.valid}};
    if (!r.valid) verified["reason"] = to_string(*r.reason);
    if (r.claims) verified["claims"] = claimsJson(*r.claims);
    EXPECT_EQ(verified, v["verify"]);
  }
}

TEST(Conformance, EntitlementsGrantACodeAsTheReferenceSays) {
  for (const json& v : group("grants")) {
    SCOPED_TRACE(nameOf(v));
    // As a lease's ent is read: leniently (entitlementsOf).
    EXPECT_EQ(keygrant::grants(entitlementsOf(v["ent"]), v["code"].get<std::string>()), v["expect"].get<bool>());
  }
}

TEST(KeySet, ReadsAJwkSetAListOneJwkOrAnX) {
  const json one = vectors()["publicJwk"];
  const std::string kid = VerifyingKey::fromJwk(one.dump()).kid();
  const std::vector<std::string> forms = {
      json{{"keys", json::array({one})}}.dump(),
      json::array({one, one}).dump(),
      one.dump(),
      one["x"].get<std::string>(),
  };
  for (const std::string& given : forms) {
    EXPECT_EQ(KeySet::fromJwk(given).kids(), std::vector<std::string>{kid}) << given;
  }
  EXPECT_THROW(KeySet::fromJwk(R"({"keys":[]})"), std::invalid_argument);
  EXPECT_THROW(KeySet::fromJwk("[]"), std::invalid_argument);
  EXPECT_THROW(KeySet::fromJwk(R"({"keys":[{"kty":"EC"}]})"), std::invalid_argument);
  json withKeys = one;
  withKeys["keys"] = "x";
  EXPECT_THROW(KeySet::fromJwk(withKeys.dump()), std::invalid_argument);
}

}  // namespace
}  // namespace kgtest
