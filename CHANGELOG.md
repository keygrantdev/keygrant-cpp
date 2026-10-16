# Changelog

## 0.7.0

- Metered uses: `License::use(meter, n = 1)` spends `n` uses of a meter the key's licence model counts (1,000
  exports for good, 50 renders each billing period) and answers a `keygrant::UseResult` (`ok`, `left`, `offline`,
  `error`, an optional `UseError`: `InvalidAmount`, `NoMeter`, `UsedUp` (with `left`), `OfflineLimit` (with `left`),
  `NotLicensed`, `Storage`, or the server's verdict on the key, the build or the device: `Revoked`, `Upgrade`, `Outdated`,
  `Device`). Online it is exact and atomic. When KeyGrant gives no answer (no network, a timeout, a 5xx, a 429) the
  device spends from its offline allowance for the meter, never past what it last heard is left, and answers `ok` with
  `offline` true; past the allowance it is `OfflineLimit`. A spend the allowance would refuse asks the server, whatever
  the last outage left behind, once a minute while it stays out of reach; one it covers waits a minute after an outage,
  doubling to an hour. It throws for no outcome (what no adapter can tell it, an adapter that threw, is `Storage`),
  blocks and takes no cancellation, and its request waits at most 5 seconds (less when `httpTimeout` is shorter). `n`
  is a `std::int64_t`: anything outside 1 to `kMaxMeterUses` (1,000,000,000) is `InvalidAmount`, asking nothing. A meter
  never takes a licence away: nothing in `status()` reads one to decide whether the app is licensed. `to_string` gives
  the words every KeyGrant SDK uses (`invalid-amount`, `used-up`, `offline-limit`, ...), and a `switch` over every
  `UseError` has ten cases. The new types are in `<keygrant/meter.hpp>`, which `<keygrant/types.hpp>` includes.
- `LicenseStatus::meters`, by meter code: `left` (the newest figure, the lease's or the last `use` answer's, net of
  this device's unsynced spends), `total`, `offline` (this device's allowance), `unsynced` and `resets`. Offline, from the
  lease and the stored figures; on a `Licensed` status from a key's lease that names meters only, empty on every other
  status. `StoredState` has `spends` and `meterFigures`, so its layout changed (the 0.x SONAME carries the minor version,
  so the library is `libkeygrant.so.0.7`).
- The spends a device made with no answer are kept in the stored state (`StoredState::spends`, with the newest figures a
  `use` answer gave in `meterFigures`), merged across processes as the rest of the state is, and reported with the next
  validate (a check-in or renewal), `use` or `deactivate`, which now also names the device's fingerprints when it carries
  a report. They are dropped only when the server says it recorded them; a device back online reports at its next
  `status()`. A journal never fails a renewal or a deactivation. Entries not reported within 180 days are dropped, and at
  most 64 are kept (no more than 16 of them the take-back of a spend whose answer was lost; a new one takes the oldest
  one's place). A
  spend the SDK could not save (a disk that refuses its writes) is kept in memory, counted in `left` and `unsynced`, and
  reported once it has been saved. The state is read leniently: a journal that is not a list, or an entry of another
  shape, is read as empty or left out.
- A lease's `met` claim is read leniently, as `ent` is: a member that is not a well-formed meter (a figure missing,
  negative, fractional, past 2^53 - 1 or out of a double's range) is dropped, never the lease. A number out of a double's
  range (`1e400`) no longer fails the text it is in: before, a lease payload, a licence file or a server answer holding
  one was refused whole (or read as no body at all); the member holding it is now read as absent. An entitlement holding
  one is dropped, where it made the lease `bad-claims`.
- A spend's id is 16 bytes from OpenSSL's random source, which the SDK links already: no new dependency.
- The conformance vectors' twelve metered-uses groups are replayed (410 of their 412 vectors: the two that give `use` a
  fraction and a text are JavaScript's only), with the captured request bodies, behaviour tests, and a seeded property
  test of two processes over one state file (1,500 sequences).

## 0.6.0

- User-locked keys: `License::activate(key, options)` and `License::importLicenseFile(content, options)`,
  overloads that take a `keygrant::ActivateOptions`. A licence model can let only the person a key was sold to
  activate a new device on it, known by the address on the key (`email`), and, for a key that mails a code to that
  address, by the code too (`code`). Both are optional strings; the existing calls are as they were and send
  neither. Only a new device is ever asked: `refresh()`, `status()`, a renewal, a seat the key already holds and
  offline behaviour are unchanged. Nothing about the address or the code is stored.
- `ActivateError` has seven more enumerators, the refusals an app can act on: `UserEmailRequired` (ask for the
  address), `UserEmailMismatch`, `UserCodeRequired` (a code was just mailed: ask for it), `UserCodeInvalid`,
  `TooManyCodes`, `TooManyAttempts` and `TryAgain` (the code could not be sent: nothing was activated, ask again).
  `to_string` gives the server's words (`user-email-required`, ...). A `switch` over every `ActivateError` has seven
  more cases.
- An address longer than 254 UTF-16 units, or a code that is not exactly six ASCII digits (spaces inside it are
  dropped first), is refused by the SDK itself, for any key, before the server is asked (`UserEmailMismatch`,
  `UserCodeInvalid`). The conformance vectors' two new groups, `typedFor` and `userLockErrors`, hold this to the
  reference. The 0.x SONAME carries the minor version, so the library is `libkeygrant.so.0.6`.

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
