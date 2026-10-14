// Licence files: an offline file imported, stored and answered with no network; every refusal; a
// stored file that does not license this build; and what this port reads on its own.
#include <string>
#include <vector>

#include "license_file.hpp"
#include "license_file_support.hpp"

namespace kgtest {
namespace {

using keygrant::ImportLicenseFileResult;
using keygrant::LicenseFileError;
using keygrant::LicenseFileKind;

ImportLicenseFileResult offlineTaken(keygrant::LicensedTo to) { return ImportLicenseFileResult::taken(LicenseFileKind::Offline, std::move(to)); }
ImportLicenseFileResult refused(LicenseFileError why) { return ImportLicenseFileResult::refused(why); }

TEST(LicenseFile, AnOfflineFileIsTakenFromItsTextWithNoNetworkStoredAndLicensesAsTheFile) {
  Harness h;
  const std::string token = offlineFile(h.key);
  auto app = fileApp(h);
  EXPECT_EQ(app->importLicenseFile(fileText(token)), offlineTaken(ada()));
  EXPECT_EQ(h.stored()->licenseFile, token);
  EXPECT_EQ(app->status(), fileLicensed());
  EXPECT_TRUE(h.http->actions().empty());
}

TEST(LicenseFile, AnOfflineFileIsTakenAsBytesAsADropHandlerReadsThem) {
  Harness h;
  const std::string token = offlineFile(h.key);
  const std::string text = fileText(token);
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  EXPECT_EQ(fileApp(h)->importLicenseFile(bytes), offlineTaken(ada()));
  EXPECT_EQ(h.stored()->licenseFile, token);
}

TEST(LicenseFile, AStoredOfflineFileNeverGoesOnlineRefreshIncludedThoughTheKeyBesideItHasRunOut) {
  Harness h;
  // Without the file, this run-out lease would be asked about, and the wait after the ask saved.
  LeaseSpec spec;
  spec.major = 3;
  spec.ttlSeconds = 3600;
  spec.now = NOW - 2 * DAY;
  StoredState state = held(h.sign(spec));
  state.licenseFile = offlineFile(h.key);
  h.store(state);
  auto app = fileApp(h);
  EXPECT_EQ(app->status(), fileLicensed());
  EXPECT_EQ(app->refresh(), fileLicensed());
  EXPECT_TRUE(h.http->actions().empty());
  EXPECT_EQ(h.storage->writes(), 0);
}

TEST(LicenseFile, AFileSaysWhenItIsATestLicenceAndCarriesItsEntitlementsWhichHasReads) {
  Harness h;
  h.store(storedFile(offlineFile(h.key, {{"test", true}, {"ent", {{"edition", "pro"}, {"cloud", true}}}})));
  auto app = fileApp(h);
  LicenseStatus expected = fileLicensed();
  expected.test = true;
  expected.entitlements = {{"edition", "pro"}, {"cloud", true}};
  EXPECT_EQ(app->status(), expected);
  EXPECT_TRUE(app->has("cloud"));
  try {
    app->require("export");
    FAIL() << "export is not granted";
  } catch (const keygrant::MissingEntitlement& missing) {
    EXPECT_EQ(missing.entitlement(), "export");
  }
}

TEST(LicenseFile, RemovingNoFileIsNoErrorAndLeavesUnlicensedAsItWas) {
  Harness h;
  auto app = fileApp(h);
  app->removeLicenseFile();
  EXPECT_EQ(app->status(), status(LicenseState::Unlicensed));
}

TEST(LicenseFile, AFileReplacesTheOneStoredBeforeAndAFileRefusedLeavesIt) {
  Harness h;
  auto app = fileApp(h);
  const std::string first = offlineFile(h.key);
  EXPECT_EQ(app->importLicenseFile(first), offlineTaken(ada()));
  EXPECT_EQ(h.stored()->licenseFile, first);
  const std::string second = offlineFile(h.key, {{"fid", "lf_3"}, {"name", "Grace Hopper"}, {"email", nullptr}});
  EXPECT_EQ(app->importLicenseFile(second), offlineTaken(keygrant::LicensedTo{"Grace Hopper", std::nullopt}));
  EXPECT_EQ(app->importLicenseFile(offlineFile(h.key, {{"product", "maindeck"}})), refused(LicenseFileError::WrongProduct));
  EXPECT_EQ(h.stored()->licenseFile, second);
}

TEST(LicenseFile, AFileRefusedNamesEachReasonAndStoresNothing) {
  Harness h;
  const std::string token = offlineFile(h.key);
  auto app = fileApp(h, {"lf_9"});
  const std::vector<std::pair<std::string, LicenseFileError>> cases = {
      {"", LicenseFileError::Malformed},
      {token + "\n" + token, LicenseFileError::Malformed},
      {std::string("\xFF\xFE\x00", 3), LicenseFileError::Malformed},
      {leaseTo3(h), LicenseFileError::NotALicenseFile},
      {payloadSwapped(token, offlineFile(h.key, {{"major", 9}})), LicenseFileError::BadSignature},
      {offlineFile(TestKey()), LicenseFileError::UnknownKey},
      {offlineFile(h.key, {{"product", "maindeck"}}), LicenseFileError::WrongProduct},
      {offlineFile(h.key, {{"fid", "lf_9"}}), LicenseFileError::RevokedFile},
      {offlineFile(h.key, {{"major", 1}}), LicenseFileError::Upgrade},
      {offlineFile(h.key, {{"minMajor", 3}}), LicenseFileError::Outdated},
  };
  for (const auto& [content, error] : cases) {
    EXPECT_EQ(app->importLicenseFile(content), refused(error)) << keygrant::to_string(error);
  }
  EXPECT_EQ(h.storage->writes(), 0);
  EXPECT_TRUE(h.http->actions().empty());
}

TEST(LicenseFile, ContentLongerThanAFileCanBeIsMalformedUnread) {
  Harness h;
  const std::string padded = std::string(keygrant::kMaxLicenseFileBytes, '#') + "\n" + offlineFile(h.key);
  EXPECT_EQ(fileApp(h)->importLicenseFile(padded), refused(LicenseFileError::Malformed));
  EXPECT_EQ(h.storage->writes(), 0);
}

TEST(LicenseFile, AnImportOrARemovalThatCannotBeSavedThrowsOneAttemptEach) {
  Harness h;
  h.store(storedFile(offlineFile(h.key)));
  auto app = fileApp(h);
  h.storage->failWrites = true;
  EXPECT_THROW(app->importLicenseFile(offlineFile(h.key, {{"fid", "lf_3"}})), keygrant::IoError);
  EXPECT_THROW(app->removeLicenseFile(), keygrant::IoError);
  EXPECT_EQ(h.storage->writes(), 2);
}

TEST(LicenseFile, AnImportOrARemovalWhoseStateCannotBeReadThrowsWritingNothing) {
  Harness h;
  auto app = fileApp(h);
  h.storage->failNextRead = true;
  EXPECT_THROW(app->importLicenseFile(offlineFile(h.key)), keygrant::IoError);
  h.storage->failNextRead = true;
  EXPECT_THROW(app->removeLicenseFile(), keygrant::IoError);
  EXPECT_EQ(h.storage->writes(), 0);
}

TEST(LicenseFile, AStoredFileIsRevokedFileOnceAnUpdateListsItWithWhoseItIsAndNoEntitlements) {
  Harness h;
  h.store(storedFile(offlineFile(h.key, {{"ent", {{"cloud", true}}}})));
  auto app = fileApp(h, {"lf_0", "lf_1"});
  EXPECT_EQ(app->status(), fileInvalid(LicenseReason::RevokedFile));
  EXPECT_FALSE(app->has("cloud"));
}

TEST(LicenseFile, AStoredFileIsUpgradeOnABuildNewerThanItCoversAndOutdatedOnOneOlder) {
  Harness h;
  h.store(storedFile(offlineFile(h.key, {{"minMajor", 2}, {"major", 3}})));
  EXPECT_EQ(h.licenseFor(4)->status(), fileInvalid(LicenseReason::Upgrade));
  EXPECT_EQ(h.licenseFor(1)->status(), fileInvalid(LicenseReason::Outdated));
}

TEST(LicenseFile, AMajorPastTheSafeRangeIsHeldAtItsEndAndOneNotFiniteOrNegativeIsMalformed) {
  // The reference compares majors as doubles; this port holds a whole number past 2^53 at that end,
  // which judges every build alike.
  Harness h;
  auto app = fileApp(h);
  h.store(storedFile(offlineFile(h.key, {{"major", 1e20}})));
  EXPECT_EQ(app->status(), fileLicensed());
  h.store(storedFile(offlineFile(h.key, {{"major", 1e20}, {"minMajor", 1e19}})));
  EXPECT_EQ(app->status(), fileInvalid(LicenseReason::Outdated));
  const std::string claims =
      R"({"iss":"keygrant","kind":"offline","fid":"lf_1","sub":"K","product":"sluice","name":"Ada","ent":{},"iat":1,"major":)";
  const auto raw = [&](const std::string& major) { return rawFile(h.key, claims + major + "}"); };
  // The control: the same claims with a real major are a file.
  EXPECT_TRUE(app->importLicenseFile(raw("3")).ok);
  EXPECT_EQ(app->importLicenseFile(raw("1e400")), refused(LicenseFileError::Malformed));
  EXPECT_EQ(app->importLicenseFile(raw("-1e20")), refused(LicenseFileError::Malformed));
}

TEST(LicenseFile, AStoredValueThatIsNotTextIsNoFileAsEveryFieldOfTheWrongTypeIs) {
  // The reference reads one as tampered; this port reads a stored field of the wrong type as absent,
  // every field alike. Either way it licenses nothing.
  Harness h;
  auto app = fileApp(h);
  for (const json& value : {json(42), json(nullptr), json{{"token", "x"}}}) {
    h.store(keygrant::parseState(json{{"licenseFile", value}}.dump()));
    EXPECT_EQ(app->status(), status(LicenseState::Unlicensed)) << value.dump();
  }
  // The control: the same field holding text is the stored file.
  h.store(keygrant::parseState(json{{"licenseFile", offlineFile(h.key)}}.dump()));
  EXPECT_EQ(app->status(), fileLicensed());
}

TEST(LicenseFile, AStoredTokenEditedOrAStoredKeyFileOrNoneAtAllIsTampered) {
  Harness h;
  const std::string edited = payloadSwapped(offlineFile(h.key), offlineFile(h.key, {{"major", 9}}));
  auto app = fileApp(h);
  for (const std::string& stored : {edited, keyFile(h.key), std::string()}) {
    h.store(storedFile(stored));
    EXPECT_EQ(app->status(), fileUnread(LicenseReason::Tampered)) << stored;
  }
}

TEST(LicenseFile, AStoredFileSurvivesARotationWhileTheBuildCarriesItsKeyAndIsUnknownKeyAfter) {
  Harness h;
  h.store(storedFile(offlineFile(h.key)));
  const TestKey next;
  const std::string both = "[" + next.publicJwk() + "," + h.key.publicJwk() + "]";
  // A build after the switch: the new key signs, the old one is kept because it signed files.
  EXPECT_EQ(fileApp(h, {}, both)->status(), fileLicensed());
  // A build that dropped it (a leaked key): the file must be signed again.
  EXPECT_EQ(fileApp(h, {}, next.publicJwk())->status(), fileUnread(LicenseReason::UnknownKey));
}

TEST(LicenseFile, TheWordsAndConstantsAreTheReferences) {
  EXPECT_EQ(keygrant::kLicenseFileExtension, ".kglicense");
  EXPECT_EQ(keygrant::kLicenseFileMediaType, "application/vnd.keygrant.license");
  EXPECT_STREQ(keygrant::to_string(LicenseReason::RevokedFile), "revoked-file");
  EXPECT_STREQ(keygrant::to_string(keygrant::LicenseSource::File), "file");
  EXPECT_STREQ(keygrant::to_string(LicenseFileKind::Key), "key");
  EXPECT_STREQ(keygrant::to_string(LicenseFileKind::Offline), "offline");
  const std::vector<std::pair<LicenseFileError, const char*>> words = {
      {LicenseFileError::Malformed, "malformed"},       {LicenseFileError::NotALicenseFile, "not-a-license-file"},
      {LicenseFileError::BadSignature, "bad-signature"}, {LicenseFileError::UnknownKey, "unknown-key"},
      {LicenseFileError::WrongProduct, "wrong-product"}, {LicenseFileError::RevokedFile, "revoked-file"},
      {LicenseFileError::Upgrade, "upgrade"},           {LicenseFileError::Outdated, "outdated"},
  };
  for (const auto& [error, word] : words) EXPECT_STREQ(keygrant::to_string(error), word);
}

TEST(LicenseFile, TheStoredFileIsLicenseFileInTheStateFileWrittenAndReadByThatName) {
  StoredState state;
  state.key = KEY;
  state.licenseFile = "a.b.c";
  const json written = json::parse(keygrant::serializeState(state));
  EXPECT_EQ(written, (json{{"key", KEY}, {"licenseFile", "a.b.c"}}));
  const std::optional<StoredState> read = keygrant::parseState(written.dump());
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->licenseFile, "a.b.c");
  EXPECT_TRUE(read->otherFields.empty());
}

TEST(LicenseFile, AContainerIsReadAsStrictUTF8EveryIllFormedSequenceRefused) {
  const TestKey key;
  const std::string token = offlineFile(key);
  const auto file = [&](const std::string& note) { return "# " + note + "\n" + token + "\n"; };
  for (const std::string& bad : {std::string("\xC0\x80"), std::string("\xE0\x9F\xBF"), std::string("\xED\xA0\x80"),
                                 std::string("\xF4\x90\x80\x80"), std::string("\xE2\x82\x41"), std::string("\xF8")}) {
    EXPECT_FALSE(keygrant::detail::licenseFileToken(file(bad)).has_value());
  }
  for (const std::string& good : {std::string("\xC2\x80"), std::string("\xE0\xA0\x80"), std::string("\xED\x9F\xBF"),
                                  std::string("\xF0\x90\x80\x80"), std::string("\xF4\x8F\xBF\xBF")}) {
    EXPECT_EQ(keygrant::detail::licenseFileToken(file(good)), token);
  }
}

}  // namespace
}  // namespace kgtest
