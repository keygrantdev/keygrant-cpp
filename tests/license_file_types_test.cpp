// The licence-file types an app compares and prints (a status from a file, the import's answer, the
// stored field), and a signed file's claims nested deeper than a copy of them could recurse.
#include <sstream>
#include <string>

#include "license_file_support.hpp"

namespace kgtest {
namespace {

using keygrant::ImportLicenseFileResult;
using keygrant::LicenseFileError;
using keygrant::LicenseFileKind;
using keygrant::LicensedTo;

template <typename T>
std::string printed(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

TEST(LicenseFileTypes, AStatusFromAFileIsNotTheKeysThoughOnlyItsSourceAndLicenseeDiffer) {
  // What tells the precedence tests' file and key answers apart.
  LicenseStatus noSource = fileLicensed();
  noSource.source.reset();
  LicenseStatus noLicensee = fileLicensed();
  noLicensee.licensedTo.reset();
  EXPECT_EQ(fileLicensed(), fileLicensed());
  EXPECT_NE(fileLicensed(), noSource);
  EXPECT_NE(fileLicensed(), noLicensee);
  EXPECT_NE(noSource, licensed(KEY));
  // A licensee that differs in either member alone.
  LicenseStatus otherName = fileLicensed();
  otherName.licensedTo->name = "Grace Hopper";
  LicenseStatus otherEmail = fileLicensed();
  otherEmail.licensedTo->email.reset();
  EXPECT_NE(fileLicensed(), otherName);
  EXPECT_NE(fileLicensed(), otherEmail);
}

TEST(LicenseFileTypes, AStatusFromAFilePrintsItsSourceAndWhoItIsLicensedTo) {
  EXPECT_EQ(printed(fileLicensed()),
            "{ state: licensed, key: SLUICE-AAAA-BBBB-CCCC-DDDD, source: file, licensedTo: { name: Ada Lovelace, "
            "email: ada@example.com } }");
  EXPECT_EQ(printed(fileUnread(LicenseReason::UnknownKey)), "{ state: invalid, reason: unknown-key, source: file }");
  EXPECT_EQ(printed(LicensedTo{std::nullopt, ADA_EMAIL}), "{ email: ada@example.com }");
  EXPECT_EQ(printed(keygrant::LicenseSource::File), "file");
}

TEST(LicenseFileTypes, AnImportsAnswerPrintsEachOfItsThreeShapes) {
  EXPECT_EQ(printed(ImportLicenseFileResult::taken(LicenseFileKind::Key, LicensedTo{ADA_NAME, std::nullopt})),
            "{ ok: true, kind: key, licensedTo: { name: Ada Lovelace } }");
  EXPECT_EQ(printed(ImportLicenseFileResult::refused(LicenseFileError::NotALicenseFile)),
            "{ ok: false, error: not-a-license-file }");
  EXPECT_EQ(printed(ImportLicenseFileResult::notActivated(ActivateError::Limit)), "{ ok: false, activateError: limit }");
  EXPECT_EQ(printed(LicenseFileKind::Offline), "offline");
  EXPECT_EQ(printed(LicenseFileError::RevokedFile), "revoked-file");
}

TEST(LicenseFileTypes, AnImportsAnswersAreEqualOnlyWhenAlikeInEveryMember) {
  const ImportLicenseFileResult taken = ImportLicenseFileResult::taken(LicenseFileKind::Offline, ada());
  EXPECT_EQ(taken, ImportLicenseFileResult::taken(LicenseFileKind::Offline, ada()));
  EXPECT_NE(taken, ImportLicenseFileResult::taken(LicenseFileKind::Key, ada()));
  EXPECT_NE(taken, ImportLicenseFileResult::taken(LicenseFileKind::Offline, LicensedTo{ADA_NAME, std::nullopt}));
  EXPECT_NE(ImportLicenseFileResult::refused(LicenseFileError::Upgrade),
            ImportLicenseFileResult::refused(LicenseFileError::Outdated));
  EXPECT_NE(ImportLicenseFileResult::notActivated(ActivateError::Limit),
            ImportLicenseFileResult::notActivated(ActivateError::Network));
  EXPECT_NE(ImportLicenseFileResult::refused(LicenseFileError::Malformed),
            ImportLicenseFileResult::notActivated(ActivateError::InvalidKey));
}

TEST(LicenseFileTypes, StatesThatDifferOnlyInTheirLicenceFileAreNotEqual) {
  StoredState one;
  one.licenseFile = "a.b.c";
  StoredState other = one;
  EXPECT_EQ(one, other);
  other.licenseFile = "a.b.d";
  EXPECT_NE(one, other);
  other.licenseFile.reset();
  EXPECT_NE(one, other);
}

TEST(LicenseFileTypes, ClaimsNestedDeepAreReadAsTheReferenceReadsThemNeverACrash) {
  // A stored file has no container's bound: only its signature stands between it and the parser.
  for (const std::size_t depth : kDeepNesting) {
    SCOPED_TRACE(depth);
    const std::string deep = nestedArrays(depth);
    const std::string claims =
        R"({"iss":"keygrant","kind":"offline","fid":"lf_1","sub":"SLUICE-AAAA-BBBB-CCCC-DDDD","product":"sluice",)"
        R"("name":"Ada Lovelace","email":"ada@example.com","major":3,"iat":1757000000,)";
    Harness h;
    auto app = fileApp(h);
    // A claim the file does not read, and an entitlement that is no flag, number or text: left out.
    h.store(storedFile(rawFile(h.key, claims + R"("ent":{"deep":)" + deep + R"(},"later":)" + deep + "}")));
    EXPECT_EQ(app->status(), fileLicensed());
    // A claim the file does read, nested where text goes: bad claims.
    h.store(storedFile(rawFile(h.key, claims + R"("ent":{},"test":)" + deep + "}")));
    EXPECT_EQ(app->status(), fileUnread(LicenseReason::Tampered));
  }
}

}  // namespace
}  // namespace kgtest
