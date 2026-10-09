// The purchase groups of conformance/vectors.json, replayed as the reference answers them from the
// JSON: every answer must match.
#include <cmath>
#include <set>

#include "conformance.hpp"
#include "offline.hpp"
#include "pickup.hpp"
#include "url.hpp"
#include "wire.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;

/** A claim's answer body as JSON says it: a member with no value left out. */
json claimBodyJson(const ClaimBody& body) {
  json out = json::object();
  if (body.key) out["key"] = *body.key;
  if (body.activationId) out["activationId"] = *body.activationId;
  if (body.lease) out["lease"] = *body.lease;
  if (body.pending) out["pending"] = true;
  if (body.error) out["error"] = *body.error;
  return out;
}

/** A claim's outcome as JSON says it: the result, the status as plain JSON (or null), the patch as a patch. */
json claimJson(const ClaimOutcome& outcome) {
  return {{"result", to_string(outcome.result)},
          {"status", outcome.status ? statusJson(*outcome.status) : json(nullptr)},
          {"patch", patchJson(outcome.patch)}};
}

TEST(ConformancePurchase, Majors) {
  int replayed = 0;
  for (const json& v : group("majors")) {
    SCOPED_TRACE(nameOf(v));
    const json& configured = v["configured"];
    // A configured major here is a whole number (std::int64_t, signed): a
    // fraction is one no config of this SDK can hold, and only a vector marked
    // jsOnly may be one. A negative is whole, and replayed.
    if (configured.is_number_float() && std::trunc(configured.get<double>()) != configured.get<double>()) {
      EXPECT_TRUE(v.value("jsOnly", false)) << "a fraction not marked jsOnly";
      continue;
    }
    const std::optional<std::int64_t> major =
        configured.is_null() ? std::nullopt : std::optional<std::int64_t>(configured.get<std::int64_t>());
    EXPECT_EQ(majorOf(major), v["expect"].get<std::int64_t>());
    ++replayed;
  }
  // None, 0, 1, 2, 7 and a negative, at least.
  EXPECT_GE(replayed, 6);
}

TEST(ConformancePurchase, PurchaseUrls) {
  for (const json& v : group("purchaseUrls")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(purchaseLink(v["link"].get<std::string>(), v["secret"].get<std::string>()),
              v["expect"].get<std::string>());
  }
}

TEST(ConformancePurchase, PurchaseReferences) {
  for (const json& v : group("purchaseReferences")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(purchaseReference(v["secret"].get<std::string>()), v["expect"].get<std::string>());
  }
}

TEST(ConformancePurchase, PurchaseTokenKept) {
  for (const json& v : group("purchaseTokenKept")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(purchaseTokenKept(stateFrom(v["state"]), v["now"].get<std::int64_t>()), v["expect"].get<bool>());
  }
}

TEST(ConformancePurchase, ClaimDue) {
  for (const json& v : group("claimDue")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(claimDue(stateFrom(v["state"]), v["clockNow"].get<std::int64_t>(), v["now"].get<std::int64_t>()),
              v["expect"].get<bool>());
  }
}

TEST(ConformancePurchase, ParseClaimBody) {
  for (const json& v : group("parseClaimBody")) {
    SCOPED_TRACE(v["body"].dump());
    EXPECT_EQ(claimBodyJson(parseClaimBody(v["body"])), v["expect"]);
  }
}

TEST(ConformancePurchase, ClaimOutcomes) {
  std::set<std::string> results;
  for (const json& v : group("claimOutcomes")) {
    SCOPED_TRACE(nameOf(v));
    ClaimInput input;
    input.response = responseFrom(v["response"]);
    input.check = checkFrom(v["check"]);
    input.clockNow = v["clockNow"].get<std::int64_t>();
    input.now = v["now"].get<std::int64_t>();
    input.major = v["major"].get<std::int64_t>();
    input.kind = kindFrom(v["kind"]);
    const ClaimOutcome outcome = claimOutcome(input);
    EXPECT_EQ(claimJson(outcome), v["expect"]);
    results.insert(to_string(outcome.result));
  }
  // Every row of the answer table is covered.
  EXPECT_EQ(results, (std::set<std::string>{"bought", "dropped", "kept"}));
}

TEST(ConformancePurchase, ClaimBodies) {
  std::set<std::string> answers;
  for (const json& v : group("claimBodies")) {
    SCOPED_TRACE(nameOf(v));
    // The device read as the devices group reads one: no install kind, held as the vector says.
    const json& given = v["identity"];
    keygrant::DeviceIdentity identity;
    identity.fingerprint = optString(given["fingerprint"]);
    identity.kind = kindFrom(given["kind"]);
    identity.alternates = given["alternates"].get<std::vector<std::string>>();
    const Device device = deviceFrom(identity, std::nullopt, v["deviceHold"].get<bool>());
    const std::optional<ClaimRequest> request =
        claimRequest(v["secret"].get<std::string>(), device, optString(v["deviceName"]), v["major"].get<std::int64_t>(),
                     v["start"].get<bool>());
    EXPECT_EQ(request ? json::parse(claimJson(*request).dump()) : json(nullptr), v["expect"]);
    answers.insert(request ? (request->fingerprints.empty() ? "a body, no list" : "a body") : "none");
  }
  // A body sent, and none.
  EXPECT_TRUE(answers.count("a body") && answers.count("none"));
}

TEST(ConformancePurchase, TrialReportDue) {
  for (const json& v : group("trialReportDue")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(trialReportDue(stateFrom(v["state"]), v["now"].get<std::int64_t>()), v["expect"].get<bool>());
  }
}

}  // namespace
}  // namespace kgtest
