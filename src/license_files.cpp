// Licence files on the engine: taking one an app was handed, removing it, and
// the status a stored one gives. Online only through activateNow, for a key
// file, as a typed key is; an offline file never.
#include "engine.hpp"

namespace keygrant::detail {

ImportLicenseFileResult Engine::importLicenseFile(std::string_view content, const ActivateOptions& options) {
  ImportLicenseFileResult result;
  serial([&](bool readOnly) {
    // A customer's own action, which writes: never behind a call that has not settled.
    if (readOnly) throw StalledError();
    result = importNow(content, options);
  });
  return result;
}

void Engine::removeLicenseFile() {
  serial([&](bool readOnly) {
    if (readOnly) throw StalledError();
    // Based on nothing: it is about no activation, so it lands as it is.
    StatePatch patch;
    patch.licenseFile = Change<std::string>::clear();
    save(patch);
  });
}

ImportLicenseFileResult Engine::importNow(std::string_view content, const ActivateOptions& options) {
  const std::optional<std::string> token = licenseFileToken(content);
  if (!token) return ImportLicenseFileResult::refused(LicenseFileError::Malformed);
  const LicenseFileCheck check = checkFile(*token);
  if (check.refused || !check.claims) {
    return ImportLicenseFileResult::refused(check.refused.value_or(LicenseFileError::Malformed));
  }
  const LicenseFileClaims& claims = *check.claims;
  if (claims.kind == LicenseFileKind::Key) {
    // As activate() would, its storage failures thrown as there.
    const ActivateResult activated = activateNow(claims.sub, options);
    if (!activated.ok) return ImportLicenseFileResult::notActivated(activated.error.value_or(ActivateError::Network));
    return ImportLicenseFileResult::taken(LicenseFileKind::Key, licensedToOf(claims));
  }
  StatePatch patch;
  patch.licenseFile = Change<std::string>::set(*token);
  save(patch);
  return ImportLicenseFileResult::taken(LicenseFileKind::Offline, licensedToOf(claims));
}

LicenseFileCheck Engine::checkFile(const std::string& token) const {
  return checkLicenseFile(verifyLicenseFile(token, keys_), product_, major_, revoked_);
}

std::optional<LicenseStatus> Engine::fileStatus(const StoredState& state) const {
  if (!state.licenseFile) return std::nullopt;
  return licenseFileStatus(checkFile(*state.licenseFile));
}

}  // namespace keygrant::detail
