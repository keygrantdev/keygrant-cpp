// What an earlier version stored to buy from the app with no key typed (a purchase secret and the two
// stamps beside it), through the whole engine over the default file storage: never sent, and gone from
// the file once the SDK saves.
#include "engine.hpp"
#include "memory_fs.hpp"
#include "support.hpp"

namespace kgtest {
namespace {

const std::string kToken = "q1w2e3r4t5y6u7i8o9p0a1s2d3f4g5h6j7k8l9z0x1c";
const char* const kRetired[] = {"purchaseToken", "purchaseTokenAt", "claimAskedAt"};

/** A memory file system holding `state` as an earlier version wrote it: with the three fields beside it. */
std::shared_ptr<MemoryFs> earlier(json state) {
  state["purchaseToken"] = kToken;
  state["purchaseTokenAt"] = NOW - DAY;
  state["claimAskedAt"] = NOW - 60'000;
  return std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), state.dump()}});
}

/** A License over the default storage on `memory`, every other adapter the harness's. */
std::unique_ptr<keygrant::License> overFile(const Harness& h, const std::shared_ptr<MemoryFs>& memory) {
  keygrant::LicenseConfig config = h.config;
  config.adapters.storage = storageOver(memory);
  return std::make_unique<keygrant::License>(config, h.timings);
}

json saved(const std::shared_ptr<MemoryFs>& memory) {
  const auto text = memory->file(memoryState());
  return text ? json::parse(*text) : json();
}

void expectLeftOut(const std::shared_ptr<MemoryFs>& memory) {
  const json file = saved(memory);
  ASSERT_TRUE(file.is_object());
  for (const char* name : kRetired) EXPECT_FALSE(file.contains(name)) << name << " is still in the file";
}

void expectNeverSent(Harness& h) {
  EXPECT_FALSE(h.http->called("claim"));
  for (const std::string& action : h.http->actions()) {
    for (const json& body : h.http->bodiesOf(action)) {
      EXPECT_EQ(body.dump().find(kToken), std::string::npos) << action << " sent the secret";
    }
  }
}

TEST(Retired, ARunningTrialNeverSendsThemAndItsFirstSaveLeavesThemOut) {
  Harness h;
  const std::string lease = h.sign("trial:fp-test", "trial", 5 * 24 * 3600);
  const auto memory = earlier(
      {{"trialStart", NOW - 2 * DAY}, {"trialEndsAt", NOW + 5 * DAY}, {"trialLease", lease}, {"lastSeen", NOW}});
  h.http->reply("trial", 200, json{{"trialStart", NOW - 2 * DAY}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", lease}});
  const auto license = overFile(h, memory);
  EXPECT_EQ(license->status().state, LicenseState::Trial);
  ASSERT_TRUE(keygrant::detail::LicenseAccess::launchReportSettled(*license, Millis{10'000}));
  // The launch report's stamp is the first save, and it carries none of them.
  EXPECT_EQ(saved(memory)["trialReportedAt"], NOW);
  expectLeftOut(memory);
  EXPECT_EQ(license->refresh().state, LicenseState::Trial);
  EXPECT_EQ(license->startTrial().state, LicenseState::Trial);
  expectLeftOut(memory);
  expectNeverSent(h);
}

TEST(Retired, AnActivationLeavesThemOutAndALaterDeactivationActivatesNothing) {
  Harness h;
  const auto memory = earlier({{"lastSeen", NOW}});
  h.http->reply("activate", 200, json{{"lease", h.sign("KEY-1", "perpetual", 30 * 24 * 3600)}, {"activationId", "act_1"}});
  h.http->reply("deactivate", 200, json::object());
  const auto license = overFile(h, memory);
  EXPECT_EQ(license->activate("KEY-1"), ok());
  expectLeftOut(memory);
  EXPECT_TRUE(license->deactivate().released);
  expectLeftOut(memory);
  EXPECT_EQ(license->status(), status(LicenseState::Unlicensed));
  expectNeverSent(h);
}

TEST(Retired, ADeactivationAloneLeavesThemOut) {
  Harness h;
  const auto memory = earlier({{"key", "KEY-1"},
                               {"activationId", "act_1"},
                               {"lease", h.sign("KEY-1", "perpetual", 30 * 24 * 3600)},
                               {"checkedMajor", 1},
                               {"lastSeen", NOW}});
  h.http->reply("deactivate", 200, json::object());
  const auto license = overFile(h, memory);
  EXPECT_EQ(license->status(), licensed("KEY-1"));
  EXPECT_TRUE(license->deactivate().released);
  expectLeftOut(memory);
  expectNeverSent(h);
}

}  // namespace
}  // namespace kgtest
