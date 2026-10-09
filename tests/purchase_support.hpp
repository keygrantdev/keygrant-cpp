// What the purchase tests (license_purchase_test.cpp, license_pickup_test.cpp)
// share: a purchase secret held, a running trial, a bought key's answer, and
// a call stuck on its read of the state.
#pragma once

#include <atomic>
#include <future>
#include <string>

#include "support.hpp"

namespace kgtest {

using namespace std::chrono_literals;

inline const std::string LINK = "https://buy.stripe.com/test_abc";
/// The purchase secret held: it stays on the device.
inline const std::string TOKEN(43, 'A');
/// What a link carries for it: kgp_ and the base64url of its SHA-256 (the purchaseReferences group's 32 zero bytes).
inline const std::string REF = "kgp_DwBzhbb51LfusnSGBa_hqYSgo7-j8BTQnip4TOnlzRo";
inline const std::string BOUGHT_KEY = "SLUICE-BUY1-BUY1-BUY1-BUY1";

/** A purchase secret held, its link handed out `ago` ms before NOW, on top of `over`. */
inline StoredState tokenHeld(std::int64_t ago = 0, StoredState over = {}) {
  over.purchaseToken = TOKEN;
  over.purchaseTokenAt = NOW - ago;
  return over;
}

/** A running trial, synced within the day (so no launch report is due), with `over`'s purchase fields on top. */
inline StoredState trialState(const Harness& h, const StoredState& over = {}) {
  StoredState state = over;
  state.trialStart = NOW - 2 * DAY;
  state.trialEndsAt = NOW + 5 * DAY;
  state.trialLease = h.sign("trial:fp-test", "trial", 5 * 24 * 3600);
  state.trialAt = NOW - DAY / 2;
  state.lastSeen = NOW;
  return state;
}

/** The server's answer when the key bought with the token is this device's: as an activation's. */
inline keygrant::HttpResponse bought(const Harness& h, const std::string& model = "perpetual") {
  const json body = {{"key", BOUGHT_KEY},
                     {"activationId", "act_1"},
                     {"lease", h.sign(BOUGHT_KEY, model, 30 * 24 * 3600)},
                     {"inGrace", false},
                     {"validUntil", nullptr}};
  return {200, body.dump()};
}

inline LicenseStatus runningTrial() {
  LicenseStatus s = status(LicenseState::Trial);
  s.trialEndsAt = NOW + 5 * DAY;
  s.trialDaysLeft = 5;
  return s;
}

/** Whether the state holds none of the purchase secret and its stamps. */
inline bool noPurchase(const std::optional<StoredState>& state) {
  return !state || (!state->purchaseToken && !state->purchaseTokenAt && !state->claimAskedAt);
}

inline HarnessOptions quick() {
  HarnessOptions o;
  o.httpTimeout = Millis{100};
  o.timings = shortTimings();
  return o;
}

/** A fingerprinter whose machine id never reads: only the host-name fingerprint, as an alternate. */
class HostOnly final : public keygrant::Fingerprinter {
 public:
  std::string get() override { return "unused"; }
  std::optional<keygrant::DeviceIdentity> identify(Millis) override {
    ++calls;
    keygrant::DeviceIdentity identity;
    identity.alternates = {"fp-host"};
    return identity;
  }
  std::atomic<int> calls{0};
};

/** A status() stuck on its read of the state, past the grace: what the License runs next runs read-only. */
inline std::future<LicenseStatus> stuckStatus(Harness& h) {
  h.storage->hangNextRead();
  auto stuck = std::async(std::launch::async, [&h] { return h.license().status(); });
  EXPECT_TRUE(h.storage->readGate().waitReached(5s));
  return stuck;
}

}  // namespace kgtest
