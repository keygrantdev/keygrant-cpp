// User-locked keys: what activate(key, options) and importLicenseFile(content, options) send for the address
// and code a person typed, how each word the server refuses a new device with comes back, and everything that
// never carries them. What the typing is prepared as, and the word-to-error mapping, are held to the
// conformance vectors (typedFor, userLockErrors); these are the calls.
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "license_file_support.hpp"
#include "support.hpp"
#include "wire.hpp"

namespace kgtest {
namespace {

using keygrant::ActivateOptions;
using keygrant::ImportLicenseFileResult;
using keygrant::LicenseFileError;
using keygrant::detail::kMaxEmail;

const std::vector<std::pair<std::string, ActivateError>> kWords = {
    {"user-email-required", ActivateError::UserEmailRequired},
    {"user-email-mismatch", ActivateError::UserEmailMismatch},
    {"user-code-required", ActivateError::UserCodeRequired},
    {"user-code-invalid", ActivateError::UserCodeInvalid},
    {"too-many-codes", ActivateError::TooManyCodes},
    {"too-many-attempts", ActivateError::TooManyAttempts},
    {"try-again", ActivateError::TryAgain},
};

json leaseAnswer(const std::string& lease) { return {{"lease", lease}, {"activationId", "act_1"}}; }

ActivateOptions typed(std::optional<std::string> email = std::nullopt, std::optional<std::string> code = std::nullopt) {
  return ActivateOptions{std::move(email), std::move(code)};
}

bool carriesNeither(const json& body) { return !body.contains("email") && !body.contains("code"); }

// --- activate with an address and a code -------------------------------------------------------

TEST(UserLock, SendsWhatThePersonTypedTrimmedBesideTheKeyAndActivateAloneSendsNothing) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_TRUE(carriesNeither(h.http->bodyOf("activate")));

  h.license().activate(KEY, typed("  Mara@Studio.example\n", " 123 456 "));
  const json body = h.http->bodiesOf("activate").back();
  EXPECT_EQ(body["key"], KEY);
  EXPECT_EQ(body["email"], "Mara@Studio.example");
  EXPECT_EQ(body["code"], "123456");
}

TEST(UserLock, TreatsABlankAddressOrCodeAsNoneAndSendsTheOneThereIsAlone) {
  Harness h;
  h.http->reply("activate", 403, {{"error", "user-email-required"}});
  h.license().activate(KEY, typed("   ", ""));
  EXPECT_TRUE(carriesNeither(h.http->bodiesOf("activate").back()));

  h.license().activate(KEY, typed("mara@studio.example"));
  const json address = h.http->bodiesOf("activate").back();
  EXPECT_EQ(address["email"], "mara@studio.example");
  EXPECT_FALSE(address.contains("code"));

  h.license().activate(KEY, typed(std::nullopt, "123456"));
  const json code = h.http->bodiesOf("activate").back();
  EXPECT_EQ(code["code"], "123456");
  EXPECT_FALSE(code.contains("email"));
}

TEST(UserLock, AnswersEachWordOfTheServerAsItsOwnErrorWhateverTheStatus) {
  for (const auto& [word, error] : kWords) {
    for (const int code : {403, 429, 503}) {
      Harness h;
      h.http->reply("activate", code, {{"error", word}});
      EXPECT_EQ(h.license().activate(KEY, typed("mara@studio.example")), failed(error)) << code << " " << word;
    }
    // The word an app sends on to its screens is the server's.
    EXPECT_STREQ(keygrant::to_string(error), word.c_str());
  }
}

TEST(UserLock, AnswersALockedKeysFirstAskWithTheKeyAloneAsTheErrorThatAsksForTheAddress) {
  Harness h;
  h.http->reply("activate", 403, {{"error", "user-email-required"}});
  EXPECT_EQ(h.license().activate(KEY), failed(ActivateError::UserEmailRequired));
}

TEST(UserLock, AnswersARateLimitOrAnOutageThatIsNotALocksWordAsNetworkAsItAlwaysDid) {
  const std::vector<std::pair<int, std::string>> cases = {
      {429, "rate-limited"}, {503, "unavailable"}, {500, "internal"}, {403, "user-something-new"}, {403, "Try-Again"}};
  for (const auto& [status, word] : cases) {
    Harness h;
    h.http->reply("activate", status, {{"error", word}});
    EXPECT_EQ(h.license().activate(KEY, typed("mara@studio.example")), failed(ActivateError::Network)) << word;
  }
  Harness down;
  down.http->fail = true;
  EXPECT_EQ(down.license().activate(KEY, typed("mara@studio.example")), failed(ActivateError::Network));
}

TEST(UserLock, StoresNothingAboutTheAddressTheCodeOrARefusal) {
  Harness h;
  h.http->reply("activate", 403, {{"error", "user-code-required"}});
  EXPECT_EQ(h.license().activate(KEY, typed("mara@studio.example", "123456")), failed(ActivateError::UserCodeRequired));
  EXPECT_EQ(h.stored(), std::nullopt);
  EXPECT_EQ(h.storage->writes(), 0);

  // Once the lease comes, the activation is stored as ever and nothing of what was typed.
  h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  EXPECT_EQ(h.license().activate(KEY, typed("mara@studio.example", "123456")), ok());
  const std::string stored = keygrant::serializeState(*h.stored());
  EXPECT_NE(stored.find(KEY), std::string::npos);
  EXPECT_EQ(stored.find("studio"), std::string::npos);
  EXPECT_EQ(stored.find("123456"), std::string::npos);
  EXPECT_EQ(h.license().status(), licensed(KEY));
}

TEST(UserLock, RefusesAnAddressTooLongAndACodeThatIsNotSixDigitsBeforeTheServerIsAsked) {
  Harness h;
  h.http->reply("activate", 403, {{"error", "user-email-required"}});
  const std::string tooLong = std::string(kMaxEmail, 'a') + "@b.example";
  EXPECT_EQ(h.license().activate(KEY, typed(tooLong)), failed(ActivateError::UserEmailMismatch));
  EXPECT_EQ(h.license().activate(KEY, typed("a@b.example", "12345")), failed(ActivateError::UserCodeInvalid));
  EXPECT_FALSE(h.http->called("activate"));

  // The address is judged first, and the key before both.
  EXPECT_EQ(h.license().activate(KEY, typed(std::string(300, 'x'), "nope")), failed(ActivateError::UserEmailMismatch));
  EXPECT_EQ(h.license().activate("", typed(std::string(300, 'x'))), failed(ActivateError::InvalidKey));
  // A blank address beside a code that is no code is still the code's fault.
  EXPECT_EQ(h.license().activate(KEY, typed("", "12")), failed(ActivateError::UserCodeInvalid));
  EXPECT_FALSE(h.http->called("activate"));
}

// --- what never carries them ---------------------------------------------------------------------

TEST(UserLock, SendsNeitherToValidateDeactivateOrATrialEvenAfterAnActivationThatCarriedThem) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", 30 * 24 * 3600)));
  h.http->reply("validate", 200, {{"lease", h.sign(KEY, "perpetual", 30 * 24 * 3600)}});
  h.http->reply("deactivate", 200, {{"ok", true}});
  EXPECT_EQ(h.license().activate(KEY, typed("mara@studio.example", "123456")), ok());
  h.license().refresh();
  h.license().deactivate();
  h.license().startTrial();
  for (const char* action : {"validate", "deactivate", "trial"}) {
    for (const json& body : h.http->bodiesOf(action)) EXPECT_TRUE(carriesNeither(body)) << action;
  }
  EXPECT_EQ(h.http->count("validate"), 1);
  EXPECT_EQ(h.http->count("deactivate"), 1);
  EXPECT_GT(h.http->count("trial"), 0);
}

// --- a key file ----------------------------------------------------------------------------------

TEST(UserLock, AsksAKeyFilesKeyWithTheSameAddressAndCodeAndItsAnswersComeBackAsActivates) {
  Harness h;
  h.http->reply("activate", 403, {{"error", "user-code-required"}});
  const std::unique_ptr<keygrant::License> app = fileApp(h);
  const std::string file = fileText(keyFile(h.key));
  EXPECT_EQ(app->importLicenseFile(file, typed(" Ada@Example.com ", "123 456")),
            ImportLicenseFileResult::notActivated(ActivateError::UserCodeRequired));
  const json body = h.http->bodyOf("activate");
  EXPECT_EQ(body["key"], KEY);
  EXPECT_EQ(body["email"], "Ada@Example.com");
  EXPECT_EQ(body["code"], "123456");

  // With no options it sends neither, as before.
  EXPECT_EQ(app->importLicenseFile(file), ImportLicenseFileResult::notActivated(ActivateError::UserCodeRequired));
  EXPECT_TRUE(carriesNeither(h.http->bodiesOf("activate").back()));
  // Nothing of it is stored.
  EXPECT_EQ(h.stored(), std::nullopt);
}

TEST(UserLock, TakesAKeyFilesOptionsAsBytesToo) {
  Harness h;
  h.http->reply("activate", 403, {{"error", "user-email-required"}});
  const std::string file = fileText(keyFile(h.key));
  const std::vector<std::uint8_t> bytes(file.begin(), file.end());
  EXPECT_EQ(fileApp(h)->importLicenseFile(bytes, typed("ada@example.com")),
            ImportLicenseFileResult::notActivated(ActivateError::UserEmailRequired));
  EXPECT_EQ(h.http->bodyOf("activate")["email"], "ada@example.com");
}

TEST(UserLock, TakesAKeyFileWhoseKeyActivatesWithTheOptionsSendingThemNormalisedAndStoringNeither) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(leaseTo3(h)));
  const ImportLicenseFileResult taken =
      fileApp(h)->importLicenseFile(fileText(keyFile(h.key)), typed("  Mara@Studio.example ", " 123 456 "));
  EXPECT_TRUE(taken.ok);
  EXPECT_EQ(taken.kind, keygrant::LicenseFileKind::Key);
  const json body = h.http->bodyOf("activate");
  EXPECT_EQ(body["key"], KEY);
  EXPECT_EQ(body["email"], "Mara@Studio.example");
  EXPECT_EQ(body["code"], "123456");
  // Activated as ever, and nothing of what was typed (nor the file's own address) is stored.
  const std::string stored = keygrant::serializeState(*h.stored());
  EXPECT_NE(stored.find(KEY), std::string::npos);
  // Whatever the case, the address as typed (its local part and its domain) and the code both as normalised and
  // as typed.
  std::string lowered = stored;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const char* typed : {"mara@", "studio.example", "123456", "123 456", "example.com"}) {
    EXPECT_EQ(lowered.find(typed), std::string::npos) << typed;
  }
}

TEST(UserLock, JudgesAKeyFilesKeyBeforeTheAddressAndTheAddressBeforeTheCode) {
  Harness h;
  h.http->reply("activate", 403, {{"error", "user-email-required"}});
  const std::unique_ptr<keygrant::License> app = fileApp(h);
  const ActivateOptions options = typed(std::string(300, 'x'), "nope");
  EXPECT_EQ(app->importLicenseFile(fileText(keyFile(h.key)), options),
            ImportLicenseFileResult::notActivated(ActivateError::UserEmailMismatch));
  EXPECT_EQ(app->importLicenseFile(fileText(keyFile(h.key, {{"sub", " "}})), options),
            ImportLicenseFileResult::notActivated(ActivateError::InvalidKey));
  EXPECT_FALSE(h.http->called("activate"));
}

TEST(UserLock, RefusesAFileThatIsNoLicenceFileBeforeAnyKeyIsAskedForWhateverComesWithIt) {
  Harness h;
  EXPECT_EQ(h.license().importLicenseFile("not a file", typed("mara@studio.example")),
            ImportLicenseFileResult::refused(LicenseFileError::Malformed));
  EXPECT_FALSE(h.http->called("activate"));
}

TEST(UserLock, IgnoresTheOptionsForAnOfflineFileAndAsksNothing) {
  Harness h;
  const ImportLicenseFileResult taken =
      fileApp(h)->importLicenseFile(fileText(offlineFile(h.key)), typed(std::string(300, 'x'), "nope"));
  EXPECT_TRUE(taken.ok);
  EXPECT_TRUE(h.http->actions().empty());
}

}  // namespace
}  // namespace kgtest
