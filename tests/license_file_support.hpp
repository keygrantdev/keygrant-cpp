// Licence-file test support: Ada's offline file and key file signed as the server signs them, a file's
// text, the statuses a file gives, and the harness's app with a denylist or another key set.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "support.hpp"

namespace kgtest {

inline constexpr const char* ADA_NAME = "Ada Lovelace";
inline constexpr const char* ADA_EMAIL = "ada@example.com";

/** A licence file signed by `key` under a file header naming it, over these exact payload bytes. */
inline std::string rawFile(const TestKey& key, const std::string& payload) {
  const std::string header = R"({"alg":"EdDSA","typ":"kglicense+jwt","kid":")" + key.kid() + R"("})";
  return key.signInput(keygrant::detail::base64UrlEncode(header) + "." + keygrant::detail::base64UrlEncode(payload));
}

/** A licence file signed by `key` under a file header naming it: `claims` with `over` laid over them (null removes one). */
inline std::string licenseFile(const TestKey& key, ordered_json claims, const ordered_json& over) {
  for (const auto& [name, value] : over.items()) {
    if (value.is_null()) {
      claims.erase(name);
    } else {
      claims[name] = value;
    }
  }
  return rawFile(key, claims.dump());
}

/** Ada's OFFLINE file (lf_1, KEY, majors 1 to 3, issued at NOW), with `over` laid over its claims. */
inline std::string offlineFile(const TestKey& key, const ordered_json& over = ordered_json::object()) {
  return licenseFile(key,
                     {{"iss", "keygrant"}, {"kind", "offline"}, {"fid", "lf_1"}, {"sub", KEY}, {"product", PRODUCT},
                      {"name", ADA_NAME}, {"email", ADA_EMAIL}, {"major", 3}, {"ent", ordered_json::object()},
                      {"iat", NOW / 1000}},
                     over);
}

/** Ada's KEY file (lf_2, KEY in lower case, her email alone), with `over` laid over it. */
inline std::string keyFile(const TestKey& key, const ordered_json& over = ordered_json::object()) {
  return licenseFile(key,
                     {{"iss", "keygrant"}, {"kind", "key"}, {"fid", "lf_2"}, {"sub", "sluice-aaaa-bbbb-cccc-dddd"},
                      {"product", PRODUCT}, {"email", ADA_EMAIL}, {"iat", NOW / 1000}},
                     over);
}

/** A licence file's text as the server writes one: notes, then the token, CRLF as a mail gateway may leave it. */
inline std::string fileText(const std::string& token) {
  return std::string("# KeyGrant license file\r\n# Product: ") + PRODUCT + "\r\n\r\n" + token + "\r\n";
}

/** `token` with its payload swapped for `other`'s, under its own header and signature. */
inline std::string payloadSwapped(const std::string& token, const std::string& other) {
  const std::size_t first = token.find('.');
  const std::size_t second = token.find('.', first + 1);
  const std::size_t otherFirst = other.find('.');
  const std::size_t otherSecond = other.find('.', otherFirst + 1);
  return token.substr(0, first + 1) + other.substr(otherFirst + 1, otherSecond - otherFirst - 1) + token.substr(second);
}

inline keygrant::LicensedTo ada() { return keygrant::LicensedTo{ADA_NAME, ADA_EMAIL}; }

/** What a status gives for Ada's offline file. */
inline LicenseStatus fileLicensed() {
  LicenseStatus s = licensed(KEY);
  s.source = keygrant::LicenseSource::File;
  s.licensedTo = ada();
  return s;
}

/** A stored file's status that does not license this build, with whose it is. */
inline LicenseStatus fileInvalid(LicenseReason reason) {
  LicenseStatus s = fileLicensed();
  s.state = LicenseState::Invalid;
  s.reason = reason;
  return s;
}

/** A stored file's status with nothing of the file read. */
inline LicenseStatus fileUnread(LicenseReason reason) {
  LicenseStatus s = status(LicenseState::Invalid, reason);
  s.source = keygrant::LicenseSource::File;
  return s;
}

/** The harness's app as major 2, its adapters kept, with `revoked` as its denylist and `publicJwk` as its key set when given. */
inline std::unique_ptr<keygrant::License> fileApp(Harness& h, std::vector<std::string> revoked = {},
                                                  const std::string& publicJwk = "") {
  keygrant::LicenseConfig config = h.config;
  config.major = 2;
  config.revokedLicenseFiles = std::move(revoked);
  if (!publicJwk.empty()) config.publicJwk = publicJwk;
  return std::make_unique<keygrant::License>(config, h.timings);
}

/** A licence lease for KEY on act_1, covering majors 1 to 3, for thirty days, signed at NOW. */
inline std::string leaseTo3(const Harness& h) {
  LeaseSpec spec;
  spec.major = 3;
  return h.sign(spec);
}

/** A state holding `token` as its licence file, and nothing else. */
inline StoredState storedFile(const std::string& token) {
  StoredState s;
  s.licenseFile = token;
  return s;
}

}  // namespace kgtest
