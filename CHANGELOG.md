# Changelog

## 0.5.0

- Licence files: `License::importLicenseFile(content)` takes a `.kglicense` file's content
  (`std::string_view` or `std::vector<std::uint8_t>`, never a path) and verifies it offline. A key file activates
  its key, as `activate(key)` would. An offline licence file is stored and licenses the app with no network, ever:
  `status()` answers it with `source == LicenseSource::File` and `licensedTo`, before any key or trial, and
  `refresh()` asks nothing for it. It answers an `ImportLicenseFileResult` (`ok` with `kind` and `licensedTo`,
  else `error`, a `LicenseFileError`, or a key file's `activateError`), and throws `StalledError` behind an
  unfinished call and the storage's exception. `removeLicenseFile()` drops it; `deactivate()` leaves it.
- `LicenseConfig::revokedLicenseFiles` lists the ids of files this build refuses: a stored one reads as `Invalid`
  with the new reason `LicenseReason::RevokedFile`. A `switch` over every `LicenseReason` has one more case.
- `LicenseStatus` has `source` and `licensedTo` (none on every status not from a file); `StoredState` has
  `licenseFile`, the stored offline file's token, so its layout changed (the 0.x SONAME carries the minor version).
- `kLicenseFileExtension` (`.kglicense`) for an open dialog's filter, `kLicenseFileMediaType` and
  `kMaxLicenseFileBytes`, in `<keygrant/license_file.hpp>` (included by `<keygrant/keygrant.hpp>`). `Entitlement`
  moved to `<keygrant/entitlement.hpp>`, which `<keygrant/types.hpp>` includes. The conformance vectors' three new
  groups hold the file reading to the reference.

## 0.4.0

- `License::purchaseUrl` is removed, and the SDK no longer asks for a bought key at `status()`, `refresh()` or
  `startTrial()`. To sell from the app, open your Payment Link from your own Buy button; the customer types the key
  from the delivery email (`activate`). A purchase secret an earlier version stored is ignored, and left out the next
  time the SDK saves its state.
- `StoredState` no longer has `purchaseToken`, `purchaseTokenAt` or `claimAskedAt`: a storage adapter or test code
  that sets them stops compiling, and the same names among its `otherFields` are never written. The shared library's
  SONAME carries the minor version while it is 0.x (`libkeygrant.so.0.4`), as `StoredState`'s layout changed.

## 0.3.0

- Key sets: `LicenseConfig::publicJwk` takes the product's key set (`{"keys":[...]}`, as `/pubkey` answers
  it) or a list of JWKs, and one key as before (a set of one). A lease naming its key (`kid`) is verified by
  that key, one naming none by each key in turn.
- A lease signed by a key the build does not carry is `Invalid` with the new reason
  `LicenseReason::UnknownKey`, rather than `Tampered`.
- Each activation, validate and claim sends the ids of the keys the build carries (`kids`).
- `License::has(code)` and `License::require(code)` answer, offline from the stored lease, whether the licence
  grants an entitlement; `require` throws `keygrant::MissingEntitlement`. `keygrant::grants` answers it of a
  status's entitlements.
- A public key's `x` is canonical base64url, as every KeyGrant SDK reads it: trailing padding is allowed;
  white space, the standard alphabet and stray bits are refused.
