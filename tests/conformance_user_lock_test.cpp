// The user-lock groups of conformance/vectors.json: what an activation sends for the address and code a
// person typed (or the error that refuses them first), and the error each word of a refused activation
// gives, each answered from its own JSON.
#include <string>

#include "conformance.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;

TEST(Conformance, TypedAddressesAndCodesAreSentAsTheReferenceSendsThemOrRefused) {
  for (const json& v : group("typedFor")) {
    SCOPED_TRACE(v["email"].dump() + " " + v["code"].dump());
    // An option that was not given is null in the file.
    const Typed typed = typedFor(optString(v["email"]), optString(v["code"]));
    json expect = json::object();
    if (typed.refused) {
      expect["refused"] = keygrant::to_string(*typed.refused);
    } else {
      json send = json::object();
      if (typed.email) send["email"] = *typed.email;
      if (typed.code) send["code"] = *typed.code;
      expect["send"] = send;
    }
    EXPECT_EQ(expect, v["expect"]);
  }
}

TEST(Conformance, TheWordsOfAUserLockAreTheirOwnErrorsAndAnyOtherIsNetwork) {
  for (const json& v : group("userLockErrors")) {
    SCOPED_TRACE(v["error"].dump());
    // Only a string error word can match: null and 42 are not one.
    EXPECT_EQ(keygrant::to_string(mapServerError(optString(v["error"]))), v["expect"].get<std::string>());
  }
}

}  // namespace
}  // namespace kgtest
