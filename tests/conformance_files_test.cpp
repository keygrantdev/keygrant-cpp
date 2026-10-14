// The licence-file groups of conformance/vectors.json: a file's container read for its token, a
// file's token verified, judged and given as a status, and a file given where a lease goes, each
// answered from its own JSON.
#include <string>
#include <vector>

#include "conformance.hpp"
#include "license_file.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;

TEST(Conformance, LicenseFileContainersHoldTheTokenTheReferenceReads) {
  for (const json& v : group("licenseFileContainers")) {
    SCOPED_TRACE(nameOf(v));
    const std::optional<Bytes> bytes = base64UrlDecode(v["bytes"].get<std::string>());
    ASSERT_TRUE(bytes.has_value());
    const std::optional<std::string> token = licenseFileToken(std::string(bytes->begin(), bytes->end()));
    EXPECT_EQ(nullOr(token), v["expect"]);
  }
}

TEST(Conformance, LicenseFilesVerifyCheckAndGiveTheStatusTheReferenceGives) {
  for (const json& v : group("licenseFiles")) {
    SCOPED_TRACE(nameOf(v));
    const FileVerifyResult r = verifyLicenseFile(v["token"].get<std::string>(), KeySet::fromJwk(v["keys"].dump()));
    json verified = {{"valid", !r.failure.has_value()}};
    if (r.failure) verified["reason"] = to_string(*r.failure);
    if (r.claims) verified["claims"] = fileClaimsJson(*r.claims);
    EXPECT_EQ(verified, v["verify"]);

    const LicenseFileCheck check = checkLicenseFile(r, v["product"].get<std::string>(), v["major"].get<std::int64_t>(),
                                                    v["revoked"].get<std::vector<std::string>>());
    const json expect = check.ok() ? json{{"ok", true}, {"kind", keygrant::to_string(check.claims->kind)},
                                          {"test", check.claims->test == true}}
                                   : json{{"ok", false}, {"reason", keygrant::to_string(*check.refused)}};
    EXPECT_EQ(expect, v["expect"]);
    EXPECT_EQ(statusJson(licenseFileStatus(check)), v["status"]);
  }
}

TEST(Conformance, LicenseFilesGivenAsLeasesAreRefusedByTheLeaseVerifierAsItIs) {
  for (const json& v : group("filesAsLeases")) {
    SCOPED_TRACE(nameOf(v));
    const VerifyResult r = verifyLease(v["token"].get<std::string>(), vectorsKey(), v["now"].get<std::int64_t>());
    json verified = {{"valid", r.valid}};
    if (!r.valid) verified["reason"] = to_string(*r.reason);
    if (r.claims) verified["claims"] = claimsJson(*r.claims);
    EXPECT_EQ(verified, v["verify"]);

    const json& h = v["holder"];
    const LeaseHolder holder{h["key"].get<std::string>(), h["activationId"].get<std::string>(),
                             h["claims"].get<std::vector<std::string>>(), h["canTell"].get<bool>()};
    const LeaseCheck check = checkLease(r, v["product"].get<std::string>(), v["major"].get<std::int64_t>(), holder);
    const json expect = check.ok ? json{{"ok", true}, {"test", check.test}} : json{{"ok", false}, {"reason", to_string(check.reason)}};
    EXPECT_EQ(expect, v["expect"]);
    EXPECT_EQ(trialLeaseValid(r, v["product"].get<std::string>(), holder.claims, holder.canTell), v["trial"].get<bool>());
  }
}

}  // namespace
}  // namespace kgtest
