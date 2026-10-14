// Licence files (.kglicense): a signed token a buyer is sent as a file, which the
// app takes by drag-and-drop or an open dialog and verifies offline.
#pragma once

#include <cstddef>
#include <optional>
#include <ostream>
#include <string_view>
#include <utility>

#include "keygrant/types.hpp"

namespace keygrant {

/// A licence file's extension, for both kinds: an open dialog's filter, a drop handler's test.
inline constexpr std::string_view kLicenseFileExtension = ".kglicense";

/// The media type of a licence file attached to an email: a vendor type, so a mail client hands it over byte for byte.
inline constexpr std::string_view kLicenseFileMediaType = "application/vnd.keygrant.license";

/// The most a licence file may be, in bytes; a larger one is Malformed. A real one is well under 2 KB.
inline constexpr std::size_t kMaxLicenseFileBytes = 16'384;

/**
 * The kind of a licence file. `Key`: it carries the licence key, which the app
 * activates online exactly as a typed key. `Offline`: a lease that never
 * expires and is bound to no device, for machines that never go online; it has
 * no seat limit and cannot be revoked, so a leaked one works until an app
 * update lists its id (LicenseConfig::revokedLicenseFiles).
 */
enum class LicenseFileKind { Key, Offline };

/**
 * Why a licence file was not taken. `Malformed`: not a licence file's text, or
 * its token or claims are broken. `NotALicenseFile`: a signed token, but not a
 * licence file (a lease, say). `BadSignature`: not signed by the key it names.
 * `UnknownKey`: signed by a key this build does not carry (a newer one, which
 * an update of the app carries). `WrongProduct`: another product's file.
 * `RevokedFile`: listed in LicenseConfig::revokedLicenseFiles. `Upgrade` /
 * `Outdated`: an offline file for older / later versions than this build.
 */
enum class LicenseFileError {
  Malformed,
  NotALicenseFile,
  BadSignature,
  UnknownKey,
  WrongProduct,
  RevokedFile,
  Upgrade,
  Outdated,
};

/// The reference's words: "key", "offline"; "malformed", "not-a-license-file", ...
const char* to_string(LicenseFileKind value) noexcept;
const char* to_string(LicenseFileError value) noexcept;
std::ostream& operator<<(std::ostream& out, LicenseFileKind value);
std::ostream& operator<<(std::ostream& out, LicenseFileError value);

/**
 * What License::importLicenseFile() did: taken (`ok`, with its `kind` and who it
 * is licensed to), refused (`error`, nothing stored), or, for a key file whose
 * key did not activate, what activate() answers (`activateError`).
 */
struct ImportLicenseFileResult {
  bool ok = false;
  std::optional<LicenseFileKind> kind;
  std::optional<LicensedTo> licensedTo;
  std::optional<LicenseFileError> error;
  std::optional<ActivateError> activateError;

  static ImportLicenseFileResult taken(LicenseFileKind kind, LicensedTo licensedTo) {
    return ImportLicenseFileResult{true, kind, std::move(licensedTo), std::nullopt, std::nullopt};
  }
  static ImportLicenseFileResult refused(LicenseFileError why) {
    return ImportLicenseFileResult{false, std::nullopt, std::nullopt, why, std::nullopt};
  }
  static ImportLicenseFileResult notActivated(ActivateError why) {
    return ImportLicenseFileResult{false, std::nullopt, std::nullopt, std::nullopt, why};
  }

  bool operator==(const ImportLicenseFileResult& other) const {
    return ok == other.ok && kind == other.kind && licensedTo == other.licensedTo && error == other.error &&
           activateError == other.activateError;
  }
  bool operator!=(const ImportLicenseFileResult& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const ImportLicenseFileResult& result);

}  // namespace keygrant
