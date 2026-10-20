// The key-pickup groups of the conformance vectors, which close conformance/vectors.json: what a pickup call sends for
// what a person typed, the error each refusal of the server is, the keys a confirm hands back as read, and the key a
// restore activates, each answered from its own JSON through the functions the engine decides with.
#include <string>
#include <vector>

#include "conformance.hpp"
#include "lease.hpp"
#include "pickup.hpp"

namespace kgtest {

const std::vector<std::string>& pickupGroups() {
  static const std::vector<std::string> groups = {"pickupAsks", "pickupRefusals", "pickedKeys", "pickupChoices"};
  return groups;
}

namespace {

/// A picked key as the vectors write it: a sale and an activation code only when there is one.
json pickedKeyJson(const keygrant::PickedKey& k) {
  json out = json::object();
  out["key"] = k.key;
  out["model"] = k.model;
  out["modelName"] = k.modelName ? json(*k.modelName) : json(nullptr);
  out["state"] = k.state;
  out["entitlements"] = keygrant::detail::entitlementsJson(k.entitlements);
  out["expiresAt"] = k.expiresAt ? json(*k.expiresAt) : json(nullptr);
  if (k.versions) {
    out["versions"] = {{"minMajor", k.versions->minMajor},
                       {"maxMajor", k.versions->maxMajor ? json(*k.versions->maxMajor) : json(nullptr)},
                       {"upgradeUntil", k.versions->upgradeUntil ? json(*k.versions->upgradeUntil) : json(nullptr)}};
  } else {
    out["versions"] = nullptr;
  }
  out["deviceLimit"] = k.deviceLimit;
  out["activeDevices"] = k.activeDevices;
  out["held"] = k.held;
  if (k.sale) out["sale"] = {{"id", k.sale->id}, {"index", k.sale->index}, {"size", k.sale->size}};
  if (k.activationCode) out["activationCode"] = *k.activationCode;
  return out;
}

using namespace keygrant::detail;

/// Every group this file answers, in the file's order, and how many vectors they hold (25, 24, 33 and 18).
const std::vector<std::string>& kGroups = pickupGroups();
constexpr std::size_t kVectors = 100;

TEST(ConformancePickup, TheHarnessAnswersEveryGroupTheVectorsHold) {
  const json& root = vectors();
  std::size_t total = 0;
  for (const std::string& name : kGroups) {
    ASSERT_TRUE(root.contains(name) && root[name].is_array()) << name << ": no such group";
    total += root[name].size();
  }
  EXPECT_EQ(total, kVectors) << "the vectors the harness answers";
}

TEST(ConformancePickup, WhatAPersonTypedForAPickupIsSentTrimmedOrRefusedBeforeAnythingIsAsked) {
  for (const json& v : group("pickupAsks")) {
    SCOPED_TRACE(v["email"].dump() + " " + v["code"].dump());
    // The code is null for the ask for a code.
    const PickupAsk ask = pickupAsk(v["email"].get<std::string>(), optString(v["code"]));
    json expect = json::object();
    if (ask.refused) {
      expect["refused"] = keygrant::to_string(*ask.refused);
    } else {
      json send = {{"email", ask.email}};
      if (ask.code) send["code"] = *ask.code;
      expect["send"] = send;
    }
    EXPECT_EQ(expect, v["expect"]);
  }
}

TEST(ConformancePickup, ARefusedCallIsTheErrorOfTheServersWordWithAWaitOnlyWhereOneHelps) {
  for (const json& v : group("pickupRefusals")) {
    SCOPED_TRACE(v["json"].dump());
    const PickupRefusal refusal = pickupRefusal(v["json"]);
    json expect = {{"error", keygrant::to_string(refusal.error)}};
    if (refusal.retryAfter) expect["retryAfter"] = *refusal.retryAfter;
    EXPECT_EQ(expect, v["expect"]);
  }
}

TEST(ConformancePickup, AConfirmsKeysAreReadLenientlyAndABodyThatIsNoAnswerIsNone) {
  for (const json& v : group("pickedKeys")) {
    SCOPED_TRACE(v["json"].dump());
    const std::optional<std::vector<keygrant::PickedKey>> keys = pickedKeysOf(v["json"]);
    json expect = nullptr;
    if (keys) {
      expect = json::array();
      for (const keygrant::PickedKey& key : *keys) expect.push_back(pickedKeyJson(key));
    }
    EXPECT_EQ(expect, v["expect"]);
  }
}

TEST(ConformancePickup, TheKeyARestoreActivatesIsTheOneHeldThenOneWithASeatThenTheFirst) {
  for (const json& v : group("pickupChoices")) {
    SCOPED_TRACE(v["keys"].dump());
    // The vectors give the keys as read: reading them again changes none.
    const std::optional<std::vector<keygrant::PickedKey>> keys = pickedKeysOf(json{{"keys", v["keys"]}});
    ASSERT_TRUE(keys.has_value());
    json again = json::array();
    for (const keygrant::PickedKey& key : *keys) again.push_back(pickedKeyJson(key));
    EXPECT_EQ(again, v["keys"]);
    const Choice choice = chosenKey(*keys, v["major"].get<std::int64_t>(), v["now"].get<std::int64_t>());
    json expect = json::object();
    if (choice.refused) {
      expect["refused"] = to_string(*choice.refused);
    } else {
      expect["index"] = *choice.index;
    }
    EXPECT_EQ(expect, v["expect"]);
  }
}

}  // namespace
}  // namespace kgtest
