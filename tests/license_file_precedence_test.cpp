// A stored offline licence file beside a key and beside a trial (one that licenses this build answers
// first, with no ask, no write and no launch report; one that does not answers only when nothing else
// does), a key file activating its key, and the calls behind one that has not finished.
#include <future>
#include <string>

#include "engine.hpp"
#include "license_file_support.hpp"

namespace kgtest {
namespace {

using keygrant::ImportLicenseFileResult;
using keygrant::LicenseFileError;
using keygrant::LicenseFileKind;
using namespace std::chrono_literals;

constexpr std::int64_t DAY_S = 86'400;

/** KEY held on act_1 with `lease`, heard on major 2, seen at NOW, beside `file` when given. */
StoredState keyed(const std::string& lease, std::optional<std::string> file = std::nullopt) {
  StoredState s = held(lease, KEY, 2);
  s.licenseFile = std::move(file);
  return s;
}

LicenseStatus keyLicensed() { return licensed(KEY); }

TEST(LicenseFilePrecedence, AFileWinsOverAHeldKeyWhichKeepsWorkingOnceTheFileIsRemoved) {
  Harness h;
  const std::string lease = leaseTo3(h);
  h.store(keyed(lease, offlineFile(h.key)));
  auto app = fileApp(h);
  EXPECT_EQ(app->status(), fileLicensed());
  app->removeLicenseFile();
  EXPECT_EQ(h.stored()->key, KEY);
  EXPECT_EQ(h.stored()->lease, lease);
  EXPECT_FALSE(h.stored()->licenseFile.has_value());
  EXPECT_EQ(app->status(), keyLicensed());
}

TEST(LicenseFilePrecedence, AFileImportedOverAHeldKeyLeavesItsSeatAskingNothing) {
  Harness h;
  const std::string lease = leaseTo3(h);
  h.store(keyed(lease));
  auto app = fileApp(h);
  const std::string file = offlineFile(h.key);
  EXPECT_EQ(app->importLicenseFile(file), ImportLicenseFileResult::taken(LicenseFileKind::Offline, ada()));
  EXPECT_EQ(h.stored()->licenseFile, file);
  EXPECT_TRUE(h.http->actions().empty());
  EXPECT_EQ(h.stored()->key, KEY);
  EXPECT_EQ(h.stored()->activationId, "act_1");
  EXPECT_EQ(h.stored()->lease, lease);
  app->removeLicenseFile();
  EXPECT_EQ(app->status(), keyLicensed());
}

TEST(LicenseFilePrecedence, AFileIsKeptByAnActivationTypedOrFromAKeyFileAndStillAnswers) {
  Harness h;
  const std::string file = offlineFile(h.key);
  h.store(storedFile(file));
  h.http->reply("activate", 200, {{"lease", leaseTo3(h)}, {"activationId", "act_1"}});
  auto app = fileApp(h);
  EXPECT_EQ(app->activate(KEY), ok());
  EXPECT_EQ(h.stored()->key, KEY);
  EXPECT_EQ(h.stored()->licenseFile, file);
  EXPECT_EQ(app->importLicenseFile(keyFile(h.key)).kind, LicenseFileKind::Key);
  EXPECT_EQ(h.stored()->licenseFile, file);
  EXPECT_EQ(app->status(), fileLicensed());
}

TEST(LicenseFilePrecedence, DeactivateLeavesAFileAloneReleasingOnlyTheKey) {
  Harness h;
  const std::string file = offlineFile(h.key);
  h.store(keyed(leaseTo3(h), file));
  h.http->reply("deactivate", 200, json::object());
  auto app = fileApp(h);
  EXPECT_EQ(app->deactivate(), keygrant::DeactivateResult{true});
  EXPECT_FALSE(h.stored()->key.has_value());
  EXPECT_EQ(h.stored()->licenseFile, file);
  EXPECT_EQ(app->status(), fileLicensed());
}

/**
 * KEY held with a second under a day left of its two-day lease (less than half its life, so due to
 * renew early), inside an open upgrade window: an unforced status asks about it and notes this major.
 */
void keyDue(Harness& h, std::optional<std::string> file) {
  LeaseSpec spec;
  spec.major = 3;
  spec.ttlSeconds = 2 * DAY_S;
  spec.now = NOW - DAY - 1000;
  spec.extra = {{"upgradeUntil", NOW / 1000 + 30 * DAY_S}};
  h.store(keyed(h.sign(spec), std::move(file)));
}

TEST(LicenseFilePrecedence, AFileThatLicensesLeavesAHeldKeyUnrenewedAndUnnotedForcedOrNot) {
  Harness h;
  keyDue(h, offlineFile(h.key));
  auto app = fileApp(h);
  EXPECT_EQ(app->status(), fileLicensed());
  EXPECT_EQ(app->refresh(), fileLicensed());
  EXPECT_TRUE(h.http->actions().empty());
  EXPECT_FALSE(h.stored()->keptMajor.has_value());
}

TEST(LicenseFilePrecedence, WithNoFileTheSameKeyIsAskedAboutAndNotedUnforcedSoTheTestAboveCanSeeIt) {
  Harness h;
  keyDue(h, std::nullopt);
  fileApp(h)->status();
  ASSERT_TRUE(h.http->waitFor("validate"));
  EXPECT_EQ(h.http->count("validate"), 1);
  EXPECT_EQ(h.stored()->keptMajor, 2);
}

/** KEY on act_1 with a stored refusal and no lease, beside the licence file `file`. */
StoredState refusedKey(std::string file) {
  StoredState s;
  s.key = KEY;
  s.activationId = "act_1";
  s.refused = keygrant::Refusal::Revoked;
  s.licenseFile = std::move(file);
  return s;
}

TEST(LicenseFilePrecedence, AFileThatLicensesAnswersOverAKeysStoredRefusalAskingNothing) {
  Harness h;
  h.store(refusedKey(offlineFile(h.key)));
  EXPECT_EQ(fileApp(h)->refresh(), fileLicensed());
  EXPECT_TRUE(h.http->actions().empty());
  EXPECT_EQ(h.storage->writes(), 0);
}

TEST(LicenseFilePrecedence, AFileThatDoesNotLicenseGivesWayToTheKey) {
  Harness h;
  h.store(keyed(leaseTo3(h), offlineFile(h.key)));
  EXPECT_EQ(fileApp(h, {"lf_1"})->status(), keyLicensed());
}

TEST(LicenseFilePrecedence, AFileThatDoesNotLicenseGivesWayToTheKeysStoredRefusalWhichIsAskedAbout) {
  Harness h;
  h.store(refusedKey(offlineFile(h.key)));
  h.http->fail = true;
  EXPECT_EQ(fileApp(h, {"lf_1"})->status(), status(LicenseState::Invalid, LicenseReason::Revoked, KEY));
  // The refusal is asked about, which a file that licenses spares (above).
  ASSERT_TRUE(h.http->waitFor("validate"));
  EXPECT_EQ(h.http->count("validate"), 1);
}

/** A trial this device started at `start`, five days long, beside `file` when given. */
StoredState trial(const Harness& h, std::int64_t start, std::optional<std::string> file, std::optional<std::int64_t> trialAt = {}) {
  StoredState s;
  s.trialStart = start;
  s.trialEndsAt = start + 5 * DAY;
  s.trialLease = h.sign("trial:fp-test", "trial", 5 * DAY_S, start);
  s.trialAt = trialAt;
  s.licenseFile = std::move(file);
  return s;
}

TEST(LicenseFilePrecedence, AFileThatLicensesAnswersBeforeARunningTrialWhoseLaunchIsThenNotReported) {
  Harness h;
  // No stamp of a report or a trial answer: a running trial would report its launch now.
  h.store(trial(h, NOW, offlineFile(h.key)));
  auto app = fileApp(h);
  EXPECT_EQ(app->status(), fileLicensed());
  EXPECT_TRUE(keygrant::detail::LicenseAccess::launchReportSettled(*app, Millis{10'000}));
  EXPECT_TRUE(h.http->actions().empty());
  EXPECT_EQ(h.storage->writes(), 0);
}

TEST(LicenseFilePrecedence, WithNoFileTheSameTrialReportsItsLaunchSoTheTestAboveCanSeeIt) {
  Harness h;
  h.store(trial(h, NOW, std::nullopt));
  auto app = fileApp(h);
  EXPECT_EQ(app->status().state, LicenseState::Trial);
  EXPECT_TRUE(keygrant::detail::LicenseAccess::launchReportSettled(*app, Millis{10'000}));
  EXPECT_EQ(h.http->count("trial"), 1);
  EXPECT_EQ(h.stored()->trialReportedAt, NOW);
}

TEST(LicenseFilePrecedence, AFileThatDoesNotLicenseGivesWayToARunningTrial) {
  Harness h;
  h.store(trial(h, NOW, offlineFile(h.key, {{"major", 1}}), NOW));
  LicenseStatus running = status(LicenseState::Trial);
  running.trialEndsAt = NOW + 5 * DAY;
  running.trialDaysLeft = 5;
  EXPECT_EQ(fileApp(h)->status(), running);
}

TEST(LicenseFilePrecedence, AFileThatDoesNotLicenseAnswersInPlaceOfAnEndedTrial) {
  Harness h;
  h.store(trial(h, NOW - 10 * DAY, offlineFile(h.key, {{"major", 1}}), NOW));
  EXPECT_EQ(fileApp(h)->status(), fileInvalid(LicenseReason::Upgrade));
  EXPECT_TRUE(h.http->actions().empty());
}

TEST(LicenseFilePrecedence, AKeyFileActivatesItsKeyOnlineAsATypedKeyJudgingNoVersionAndStoresNoFile) {
  Harness h;
  const std::string lease = leaseTo3(h);
  h.http->reply("activate", 200, {{"lease", lease}, {"activationId", "act_1"}});
  auto app = fileApp(h);
  // Versions on a key file are not read: this build is outside them.
  const std::string file = keyFile(h.key, {{"major", 1}, {"minMajor", 1}});
  EXPECT_EQ(app->importLicenseFile(fileText(file)),
            ImportLicenseFileResult::taken(LicenseFileKind::Key, keygrant::LicensedTo{std::nullopt, ADA_EMAIL}));
  // The key as a typed one is sent: normalised.
  EXPECT_EQ(h.http->bodyOf("activate")["key"], KEY);
  EXPECT_EQ(h.http->bodyOf("activate")["major"], 2);
  EXPECT_EQ(h.stored()->key, KEY);
  EXPECT_EQ(h.stored()->activationId, "act_1");
  EXPECT_EQ(h.stored()->lease, lease);
  EXPECT_FALSE(h.stored()->licenseFile.has_value());
  EXPECT_EQ(app->status(), keyLicensed());
}

TEST(LicenseFilePrecedence, AKeyFileAnswersWhatActivateAnswersStoringNothingWhenTheKeyDoesNotActivate) {
  Harness h;
  h.http->reply("activate", 403, {{"error", "limit"}});
  EXPECT_EQ(fileApp(h)->importLicenseFile(keyFile(h.key)), ImportLicenseFileResult::notActivated(ActivateError::Limit));
  h.http->fail = true;
  EXPECT_EQ(fileApp(h)->importLicenseFile(keyFile(h.key)), ImportLicenseFileResult::notActivated(ActivateError::Network));
  EXPECT_FALSE(h.stored().has_value());
}

TEST(LicenseFilePrecedence, AKeyFileThrowsWhereActivateDoesTheStateUnreadableAskingNothing) {
  Harness h;
  auto app = fileApp(h);
  h.storage->failReads = true;
  EXPECT_THROW(app->importLicenseFile(keyFile(h.key)), keygrant::IoError);
  EXPECT_TRUE(h.http->actions().empty());
}

TEST(LicenseFilePrecedence, AKeyFileThrowsWhereActivateDoesTheActivationNotSaved) {
  Harness h;
  h.http->reply("activate", 200, {{"lease", leaseTo3(h)}, {"activationId", "act_1"}});
  auto app = fileApp(h);
  h.storage->failWrites = true;
  EXPECT_THROW(app->importLicenseFile(keyFile(h.key)), keygrant::IoError);
  // The server answered, and the activation's save was tried: it is that save that failed.
  EXPECT_EQ(h.http->count("activate"), 1);
  EXPECT_GE(h.storage->writes(), 1);
}

TEST(LicenseFilePrecedence, AKeyFileNotForThisAppIsRefusedBeforeAnyRequest) {
  Harness h;
  auto app = fileApp(h, {"lf_2"});
  EXPECT_EQ(app->importLicenseFile(keyFile(h.key, {{"product", "maindeck"}})),
            ImportLicenseFileResult::refused(LicenseFileError::WrongProduct));
  EXPECT_EQ(app->importLicenseFile(keyFile(h.key)), ImportLicenseFileResult::refused(LicenseFileError::RevokedFile));
  EXPECT_TRUE(h.http->actions().empty());
}

TEST(LicenseFilePrecedence, BehindACallThatHasNotFinishedImportAndRemoveThrowAndAStatusAnswersTheFile) {
  HarnessOptions quick;
  quick.major = 2;
  quick.httpTimeout = Millis{100};
  quick.timings = shortTimings();
  Harness h(std::nullopt, quick);
  const std::string token = offlineFile(h.key);
  h.store(storedFile(token));
  h.storage->hangNextRead();
  auto stuck = std::async(std::launch::async, [&h] { return h.license().status(); });
  ASSERT_TRUE(h.storage->readGate().waitReached(5s));
  EXPECT_THROW(h.license().importLicenseFile(token), keygrant::StalledError);
  EXPECT_THROW(h.license().removeLicenseFile(), keygrant::StalledError);
  EXPECT_EQ(h.license().status(), stalled(fileLicensed()));
  EXPECT_EQ(h.storage->writes(), 0);
  h.storage->releaseAll();
  finish(stuck);
}

}  // namespace
}  // namespace kgtest
