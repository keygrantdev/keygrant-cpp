#include "license_file.hpp"

#include <algorithm>

#include "lease.hpp"
#include "utf.hpp"

namespace keygrant {

const char* to_string(LicenseFileKind value) noexcept {
  switch (value) {
    case LicenseFileKind::Key: return "key";
    case LicenseFileKind::Offline: return "offline";
  }
  return "offline";
}

const char* to_string(LicenseFileError value) noexcept {
  switch (value) {
    case LicenseFileError::Malformed: return "malformed";
    case LicenseFileError::NotALicenseFile: return "not-a-license-file";
    case LicenseFileError::BadSignature: return "bad-signature";
    case LicenseFileError::UnknownKey: return "unknown-key";
    case LicenseFileError::WrongProduct: return "wrong-product";
    case LicenseFileError::RevokedFile: return "revoked-file";
    case LicenseFileError::Upgrade: return "upgrade";
    case LicenseFileError::Outdated: return "outdated";
  }
  return "malformed";
}

std::ostream& operator<<(std::ostream& out, LicenseFileKind value) { return out << to_string(value); }
std::ostream& operator<<(std::ostream& out, LicenseFileError value) { return out << to_string(value); }

std::ostream& operator<<(std::ostream& out, const ImportLicenseFileResult& result) {
  if (!result.ok) {
    out << "{ ok: false";
    if (result.error) out << ", error: " << *result.error;
    if (result.activateError) out << ", activateError: " << *result.activateError;
    return out << " }";
  }
  out << "{ ok: true";
  if (result.kind) out << ", kind: " << *result.kind;
  if (result.licensedTo) out << ", licensedTo: " << *result.licensedTo;
  return out << " }";
}

namespace detail {

namespace {

constexpr std::string_view kFileHeaderStart = R"({"alg":"EdDSA","typ":"kglicense+jwt","kid":")";

/** Where a claim stands: absent, or present with its value (which may be null). */
const json* claim(const json& object, const char* name) {
  const auto it = object.find(name);
  return it == object.end() ? nullptr : &*it;
}

/// A non-empty string claim, read as the reference's JSON reader gives it.
std::optional<std::string> filled(const json* value) {
  if (value == nullptr || !value->is_string()) return std::nullopt;
  std::string text = sanitizeUtf8(value->get_ref<const std::string&>());
  if (text.empty()) return std::nullopt;
  return text;
}

/// An optional claim: true when absent (out left none) or a non-empty string; false otherwise, null included.
bool optionalText(const json& payload, const char* name, std::optional<std::string>& out) {
  const json* value = claim(payload, name);
  if (value == nullptr) return true;
  out = filled(value);
  return out.has_value();
}

/// A whole number of at least `minimum`, held to the safe range; none otherwise.
std::optional<std::int64_t> whole(const json* value, std::int64_t minimum) {
  if (value == nullptr) return std::nullopt;
  const std::optional<std::int64_t> n = wholeNumber(*value);
  if (!n || *n < minimum) return std::nullopt;
  return n;
}

/// An offline file's versions and entitlements onto `c`: false when they do not hold.
bool readOffline(const json& payload, LicenseFileClaims& c) {
  const std::optional<std::int64_t> major = whole(claim(payload, "major"), 1);
  if (!major) return false;
  c.major = *major;
  if (const json* minMajor = claim(payload, "minMajor")) {
    c.minMajor = whole(minMajor, 1);
    if (!c.minMajor) return false;
  }
  // Read leniently, never refused: absent or malformed is none.
  if (const json* ent = claim(payload, "ent")) c.ent = entitlementsOf(*ent);
  return true;
}

}  // namespace

const char* to_string(FileVerifyFailure failure) noexcept {
  switch (failure) {
    case FileVerifyFailure::Malformed: return "malformed";
    case FileVerifyFailure::NotALicenseFile: return "not-a-license-file";
    case FileVerifyFailure::UnknownKey: return "unknown-key";
    case FileVerifyFailure::BadSignature: return "bad-signature";
    case FileVerifyFailure::BadClaims: return "bad-claims";
  }
  return "malformed";
}

std::optional<std::string> licenseFileToken(std::string_view content) {
  if (content.size() > kMaxLicenseFileBytes || !isValidUtf8(content)) return std::nullopt;
  if (content.substr(0, 3) == "\xEF\xBB\xBF") content.remove_prefix(3);
  std::optional<std::string> token;
  std::size_t start = 0;
  while (start <= content.size()) {
    const std::size_t end = std::min(content.find('\n', start), content.size());
    std::string_view line = content.substr(start, end - start);
    const auto edge = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!line.empty() && edge(line.front())) line.remove_prefix(1);
    while (!line.empty() && edge(line.back())) line.remove_suffix(1);
    if (!line.empty() && line.front() != '#') {
      if (token) return std::nullopt;
      token = std::string(line);
    }
    start = end + 1;
  }
  return token;
}

std::optional<LicenseFileClaims> parseFileClaims(const json& payload) {
  if (!payload.is_object()) return std::nullopt;
  LicenseFileClaims c;
  const std::optional<std::string> kind = filled(claim(payload, "kind"));
  if (kind != "key" && kind != "offline") return std::nullopt;
  c.kind = *kind == "key" ? LicenseFileKind::Key : LicenseFileKind::Offline;
  if (filled(claim(payload, "iss")) != "keygrant") return std::nullopt;
  std::optional<std::string> fid = filled(claim(payload, "fid"));
  std::optional<std::string> sub = filled(claim(payload, "sub"));
  std::optional<std::string> product = filled(claim(payload, "product"));
  if (!fid || !sub || !product) return std::nullopt;
  c.fid = std::move(*fid);
  c.sub = std::move(*sub);
  c.product = std::move(*product);
  if (!optionalText(payload, "name", c.name) || !optionalText(payload, "email", c.email)) return std::nullopt;
  if (!c.name && !c.email) return std::nullopt;
  const std::optional<std::int64_t> iat = whole(claim(payload, "iat"), 0);
  if (!iat) return std::nullopt;
  c.iat = *iat;
  if (const json* test = claim(payload, "test")) {
    if (!test->is_boolean()) return std::nullopt;
    c.test = test->get<bool>();
  }
  if (c.kind == LicenseFileKind::Offline && !readOffline(payload, c)) return std::nullopt;
  return c;
}

json fileClaimsJson(const LicenseFileClaims& c) {
  json out = json::object();
  out["iss"] = "keygrant";
  out["kind"] = to_string(c.kind);
  out["fid"] = c.fid;
  out["sub"] = c.sub;
  out["product"] = c.product;
  if (c.name) out["name"] = *c.name;
  if (c.email) out["email"] = *c.email;
  if (c.kind == LicenseFileKind::Offline) {
    out["major"] = c.major;
    if (c.minMajor) out["minMajor"] = *c.minMajor;
    out["ent"] = entitlementsJson(c.ent);
  }
  out["iat"] = c.iat;
  if (c.test) out["test"] = *c.test;
  return out;
}

namespace {

FileVerifyResult failed(FileVerifyFailure failure) { return FileVerifyResult{failure, std::nullopt}; }

}  // namespace

FileVerifyResult verifyLicenseFile(std::string_view token, const KeySet& keys) {
  const std::size_t first = token.find('.');
  const std::size_t second = first == std::string_view::npos ? first : token.find('.', first + 1);
  if (second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos) {
    return failed(FileVerifyFailure::Malformed);
  }
  const std::optional<std::string> kid = namedKeyId(token.substr(0, first), kFileHeaderStart);
  if (!kid) return failed(FileVerifyFailure::NotALicenseFile);
  const std::optional<Bytes> signature = base64UrlDecode(token.substr(second + 1));
  if (!signature) return failed(FileVerifyFailure::Malformed);
  const auto named = std::find_if(keys.keys().begin(), keys.keys().end(),
                                  [&](const VerifyingKey& key) { return key.kid() == *kid; });
  if (named == keys.keys().end()) return failed(FileVerifyFailure::UnknownKey);
  // Only the key it names; a signature of any length but 64 bytes verifies nothing.
  if (!named->verify(sanitizeUtf8(token.substr(0, second)), *signature)) return failed(FileVerifyFailure::BadSignature);
  const std::optional<Bytes> payload = base64UrlDecode(token.substr(first + 1, second - first - 1));
  std::optional<LicenseFileClaims> claims;
  if (payload) claims = parseFileClaims(parseJsonText(std::string(payload->begin(), payload->end())));
  if (!claims) return failed(FileVerifyFailure::BadClaims);
  return FileVerifyResult{std::nullopt, std::move(claims)};
}

LicenseFileCheck checkLicenseFile(const FileVerifyResult& verified, const std::string& product, std::int64_t major,
                                  const std::vector<std::string>& revoked) {
  if (!verified.claims) {
    switch (verified.failure.value_or(FileVerifyFailure::Malformed)) {
      case FileVerifyFailure::UnknownKey: return LicenseFileCheck{LicenseFileError::UnknownKey, std::nullopt};
      case FileVerifyFailure::BadSignature: return LicenseFileCheck{LicenseFileError::BadSignature, std::nullopt};
      case FileVerifyFailure::NotALicenseFile: return LicenseFileCheck{LicenseFileError::NotALicenseFile, std::nullopt};
      default: return LicenseFileCheck{LicenseFileError::Malformed, std::nullopt};
    }
  }
  const LicenseFileClaims& c = *verified.claims;
  const auto refused = [&](LicenseFileError why) { return LicenseFileCheck{why, c}; };
  if (c.product != product) return refused(LicenseFileError::WrongProduct);
  if (std::find(revoked.begin(), revoked.end(), c.fid) != revoked.end()) return refused(LicenseFileError::RevokedFile);
  if (c.kind == LicenseFileKind::Offline) {
    if (major < c.minMajor.value_or(1)) return refused(LicenseFileError::Outdated);
    if (major > c.major) return refused(LicenseFileError::Upgrade);
  }
  return LicenseFileCheck{std::nullopt, c};
}

LicensedTo licensedToOf(const LicenseFileClaims& claims) { return LicensedTo{claims.name, claims.email}; }

LicenseStatus licenseFileStatus(const LicenseFileCheck& check) {
  LicenseStatus status;
  status.state = LicenseState::Invalid;
  status.source = LicenseSource::File;
  const std::optional<LicenseFileClaims>& c = check.claims;
  if (check.ok() && c && c->kind == LicenseFileKind::Offline) {
    status.state = LicenseState::Licensed;
    status.key = c->sub;
    status.licensedTo = licensedToOf(*c);
    status.test = c->test == true;
    status.entitlements = c->ent;
    return status;
  }
  const std::optional<LicenseFileError> why = check.refused;
  const bool read = why == LicenseFileError::RevokedFile || why == LicenseFileError::Upgrade ||
                    why == LicenseFileError::Outdated;
  if (c && read) {
    status.reason = why == LicenseFileError::RevokedFile ? LicenseReason::RevokedFile
                    : why == LicenseFileError::Upgrade   ? LicenseReason::Upgrade
                                                         : LicenseReason::Outdated;
    status.key = c->sub;
    status.licensedTo = licensedToOf(*c);
    return status;
  }
  status.reason = why == LicenseFileError::UnknownKey ? LicenseReason::UnknownKey : LicenseReason::Tampered;
  return status;
}

}  // namespace detail
}  // namespace keygrant
