# KeyGrant for C++

Licensing for native desktop apps (Qt, wxWidgets, JUCE, plain Win32, Cocoa or GTK): a licence is proven to the app by a
signed lease (a JWT, EdDSA over Ed25519) that the SDK verifies **offline** against your product's public key. The app
only talks to KeyGrant to activate, to renew a lease near or past its end, to report a newer major version, to
spend a metered use, or to report a running trial's launch (at most once a day).

The rule every line of it serves: **licensing failure fails in favour of the
paying customer.** An outage, no network, a timeout, a 5xx, a 429, an ambiguous
4xx, a body it cannot use, or a disk it cannot read just now never takes a
licence away while the device holds a valid lease. Only an explicit verdict
from the server (revoked, upgrade or update required, another device) or the
lease's own end does.

This is a port of the Electron SDK (`@keygrant/electron`, the reference), held to the same behaviour by every answer in
`conformance/vectors.json` (every vector is replayed by the suite but the `majors` fractions, which no `std::int64_t`
major can hold, and the two metered `useAmounts` that give `use` a fraction and a text, which no `std::int64_t` can). The full guide is at https://keygrant.dev/docs.

- C++17, one CMake target: `keygrant::keygrant` (static by default; `BUILD_SHARED_LIBS` honoured).
- Depends on OpenSSL 3 (Ed25519, SHA-256), libcurl (HTTP) and nlohmann/json (private: never in a public header).
- Windows, macOS and Linux: MSVC 2022 (`/W4 /WX`, as a static library over the static CRT or as a DLL), Apple Clang 15
  or later and GCC 12 or later (`-Werror`).

## Install

### CMake, from source

```cmake
include(FetchContent)
FetchContent_Declare(keygrant
  GIT_REPOSITORY https://github.com/keygrantdev/keygrant-cpp.git
  GIT_TAG        0.8.0)
FetchContent_MakeAvailable(keygrant)

target_link_libraries(my_app PRIVATE keygrant::keygrant)
```

or, with the repository vendored, `add_subdirectory(third_party/keygrant)`. Built inside your project the
SDK builds no tests and never adds `-Werror` to your build.

OpenSSL 3, libcurl and nlohmann/json must be findable by `find_package`. Installed (`cmake --install`), the SDK
is a package: `find_package(keygrant 0.8 REQUIRED)` (a static install finds OpenSSL and CURL again for you).

### vcpkg (Windows, macOS)

The SDK carries a manifest (`vcpkg.json`): `openssl`, `curl`, `nlohmann-json`, and `gtest` for the `tests`
feature. In your own manifest add the first three, then configure with vcpkg's toolchain:

```sh
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

On Linux the distribution's packages do (`libssl-dev libcurl4-openssl-dev nlohmann-json3-dev`, and
`libgtest-dev` for the tests).

## Use it

Every call **blocks**, so make them off your UI thread. A status that needs no server (the common case: a valid
lease) answers at once; one that asks is held to its bound (5 s for a check `status()` makes on a valid lease,
the HTTP bound, 15 s by default, for anything else), and the first call of a process may spend up to 2 s reading
the machine id.

```cpp
#include <keygrant/keygrant.hpp>

keygrant::LicenseConfig config;
config.product = "sluice";                          // your product's slug
config.apiBaseUrl = "https://api.keygrant.dev";
config.publicJwk = R"({"kty":"OKP","crv":"Ed25519","x":"..."})";  // see "Embedding the public key"
config.major = 3;                                   // this build's major version (a 0.x build: 0, read as 1)
config.deviceName = "Rae's MacBook";                // optional, shown in the device list

keygrant::License license(config);                  // throws std::invalid_argument on a bad config

keygrant::LicenseStatus status = license.status();
switch (status.state) {
  case keygrant::LicenseState::Licensed:   /* run; status.test is true for a test licence */ break;
  case keygrant::LicenseState::Trial:      /* run; status.trialDaysLeft */                    break;
  case keygrant::LicenseState::Unknown:    /* run, and ask again shortly: the disk said no */ break;
  case keygrant::LicenseState::Unlicensed: /* offer activate() or startTrial() */             break;
  case keygrant::LicenseState::Expired:    /* status.reason: offline (renew) or trial */      break;
  case keygrant::LicenseState::Invalid:    /* status.reason says why (below) */               break;
}

keygrant::ActivateResult result = license.activate(keyTheCustomerTyped);
if (!result.ok) showError(keygrant::to_string(*result.error));   // "invalid-key", "limit", ...
```

### Qt

```cpp
// A QObject living on the UI thread; the call runs on Qt's thread pool. license_ is a
// std::shared_ptr<keygrant::License>: the work holds its own copy, so closing the window mid-call is safe.
QFuture<keygrant::LicenseStatus> future = QtConcurrent::run([license = license_] { return license->status(); });
auto* watcher = new QFutureWatcher<keygrant::LicenseStatus>(this);
connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
  applyLicence(watcher->result());   // back on the UI thread
  watcher->deleteLater();
});
watcher->setFuture(future);
```

wxWidgets (`std::thread` + `CallAfter`), JUCE (`juce::Thread::launch` + `MessageManager::callAsync`) and plain
Win32/Cocoa/GTK work the same way: call on a worker, hand the answer to the UI thread. Calls on one `License` may
come from any number of threads (below).

### What a status says

| `state` | `reason` | What to do |
|---|---|---|
| `Licensed` | | Run. `test` is true on a test licence: refuse it in a release build if you want to. `entitlements` is what it grants (below); `meters` is what it counts ([below](#metered-uses-counting-what-a-key-may-do)). With `source == LicenseSource::File`, an offline licence file licenses it: show `licensedTo` (below). |
| `Trial` | | Run. `trialEndsAt` (ms) is the end the trial's lease was signed to, and `trialDaysLeft` counts down to it. `entitlements` is what the trial's lease grants. |
| `Expired` | `Offline` | The lease ran out and the server could not be reached: show "connect to renew". |
| `Expired` | `Trial` | The trial is over: its signed trial lease is no longer valid. |
| `Invalid` | `Revoked` | The key was revoked, expired, or this device released. |
| `Invalid` | `Upgrade` | This build is newer than the key covers: buy an upgrade (`upgradeUrl`). From a file (`source == LicenseSource::File`), newer than the file covers: the customer needs a new file, as `upgradeUrl` needs an activation. |
| `Invalid` | `Outdated` | This build is older than the key's lowest version (or the file's): update the app. |
| `Invalid` | `Device` | The licence was activated on another device: activate on this one. |
| `Invalid` | `Tampered` | The stored lease is not this licence (another key, product or signer). From a file, the stored file was edited or is not one this build can read: import it again. |
| `Invalid` | `RevokedFile` | The stored offline licence file is one this build lists as revoked (`revokedLicenseFiles`). |
| `Invalid` | `UnknownKey` | The stored lease is signed by a key this build does not carry. Online, the server renews it with the key it signs with now; when it stays so online, update the app. From a file, only an app update that carries its key helps. |
| `Unknown` | `Storage` | The state file could not be read just now: keep running, ask again. |
| `Unlicensed` | | Nothing activated, no trial. |

`stalled` is true when the answer came from storage behind a call that has not finished (below).

`activate()` errors: `InvalidKey`, `Limit` (no seat left), `Revoked`, `Upgrade`, `Outdated`, `Network`
(no answer, or one it could not use: try again), `InvalidLease` (the server's lease did not verify here: a wrong
`publicJwk` or `product`), and the seven errors of a user-locked key ([below](#user-locked-keys)). `NotConfigured` is
named for parity and never returned.

### The calls

- `activate(key)`, `activate(key, options)`: the key is trimmed and upper-cased first; empty or over 120 characters is
  `InvalidKey` without asking. Throws when the stored state cannot be read or saved (the storage's exception, e.g.
  `keygrant::IoError`). `options` is what a person typed for a user-locked key ([below](#user-locked-keys)).
- `status(forceOnline = false)`, `refresh()` (= `status(true)`): never throw. A trial whose lease does not verify
  while its stored end is still ahead (a lease signed before the product's key was rotated, say) is asked for again,
  under the usual waits and bounded as a status check is; its answer stands, and an ask that brings back no trial
  this build can verify waits a minute (no answer) or an hour (an answer) before the next.
- `deactivate()`: releases the seat and drops everything but the trial evidence, the clock guard and a stored offline
  licence file (`removeLicenseFile()` drops that), answering
  `DeactivateResult{released}`: `true` when KeyGrant answered the release with a 200, or when no key and activation
  were held; `false` when it could not be asked or answered anything else (a 5xx, a 429, a proxy's page). Then the
  seat may still be held: say so (the developer's dashboard or support can release it), rather than tell the
  customer it is free. The local licence is dropped either way. An activation that is another device's (a state
  copied or restored from another PC: its lease, valid or run out, held to a machine this run can tell is not this
  one, or a device refusal that stands for this one) is dropped here without asking KeyGrant to release it, which
  would lock that PC out; `released` is then `true`, as this device held no seat. Throws `keygrant::StalledError`
  behind a stuck call (nothing released or dropped), and the storage's exception.
- `use(meter, n = 1)`: spends `n` uses of a meter the key's licence model counts, exact online and from an offline
  allowance with no answer, and answers a `UseResult` (`ok`, `left`, `offline`, `error`, a `UseError`). It never throws
  ([below](#metered-uses-counting-what-a-key-may-do)).
- `pickUp(email)`, `pickUp(email, code)`, `restoreByEmail(email, code)`: restore a licence by the address its key was sold
  to, for a build that may not take a typed key; none stores anything but the activation a restore makes
  ([below](#restoring-a-licence-without-the-key-app-store-builds)).
- `startTrial()`: never throws. A trial runs only while its trial lease (signed by KeyGrant to the trial's own end)
  is valid: the end stored beside it is unsigned, and editing it extends nothing. An answer whose trial lease this
  build cannot verify (signed with a key the product rotated to ahead of this build, say) never replaces a stored one
  it can, which would end a running trial; an answer with no lease (the trial is over) is taken as it is. A trial the
  disk refused is kept for this process and saved by the next call that can.
- `upgradeUrl(paymentLink)`: your Stripe payment link with `client_reference_id` set to this device's
  **activation id** (never the key), or `std::nullopt` with no activation. An upgrade sale finds the key through it.
  The link's own parameters are kept as written and in their order, any `client_reference_id` already on it is
  replaced, and the fragment is kept. Throws `std::invalid_argument` for a link that is not an absolute URL.

### Licence files: drag and drop

A licence can come as a file (`.kglicense`) the customer drops onto the app or opens from its licence screen. A
**key file** carries their key: importing it activates the key online, exactly as typing it would. An **offline
licence file** licenses the app with no network at all, for good, on machines that never connect; it has no seat
limit and cannot be revoked, so the only way to stop one that leaked is an app update that lists its id:

```cpp
config.revokedLicenseFiles = {"lf_8Hq2"};   // ids of files that leaked: an update adds them here
```

In Qt, take the file from a drop on the window or from an open dialog, read at most a licence file's size, and
import it off the UI thread:

```cpp
#include <keygrant/keygrant.hpp>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeData>
#include <QtConcurrent>

namespace {
/// The first dropped local file that is a licence file (`keygrant::kLicenseFileExtension`), else an empty string.
QString licenseFileIn(const QMimeData* mime) {
  for (const QUrl& url : mime->urls()) {
    if (url.isLocalFile() && url.toLocalFile().endsWith(QStringLiteral(".kglicense"), Qt::CaseInsensitive))
      return url.toLocalFile();
  }
  return {};
}

/// Reads the file, never more than a licence file can be (a larger one is refused unread), and imports it.
keygrant::ImportLicenseFileResult importFile(keygrant::License& license, const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("the file cannot be opened");
  const QByteArray read = file.read(static_cast<qint64>(keygrant::kMaxLicenseFileBytes) + 1);
  return license.importLicenseFile(std::vector<std::uint8_t>(read.begin(), read.end()));
}
}  // namespace

// In the window's constructor: setAcceptDrops(true);
void LicenceWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (!licenseFileIn(event->mimeData()).isEmpty()) event->acceptProposedAction();
}

void LicenceWindow::dropEvent(QDropEvent* event) {
  const QString path = licenseFileIn(event->mimeData());
  if (path.isEmpty()) return;
  event->acceptProposedAction();
  importFrom(path);
}

void LicenceWindow::openLicenceFile() {   // "Open licence file..." on the licence screen
  const QString path = QFileDialog::getOpenFileName(this, tr("Open licence file"), {},
                                                    tr("Licence files (*.kglicense)"));
  if (!path.isEmpty()) importFrom(path);
}

void LicenceWindow::importFrom(const QString& path) {
  auto* watcher = new QFutureWatcher<keygrant::ImportLicenseFileResult>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
    try {
      const keygrant::ImportLicenseFileResult result = watcher->result();
      if (result.ok) showLicensedTo(*result.licensedTo);                        // "Licensed to Ada Lovelace"
      else if (result.error) showError(keygrant::to_string(*result.error));     // "revoked-file", "upgrade", ...
      else showError(keygrant::to_string(*result.activateError));               // a key file: what activate() says
    } catch (const std::exception&) {
      showError("try-again");   // unreadable, a call before it has not finished, or the state not saved
    }
    watcher->deleteLater();
  });
  // Read and imported off the UI thread (a file on a network share can be slow); license_ is a
  // std::shared_ptr<keygrant::License>, held by the work.
  watcher->setFuture(QtConcurrent::run([license = license_, path] { return importFile(*license, path); }));
}
```

wxWidgets (`wxFileDropTarget`), Win32 (`WM_DROPFILES`) and Cocoa (`NSDraggingDestination`) hand over a path the same
way: read the file there, and give `importLicenseFile` its bytes.

`importLicenseFile` takes the file's content (`std::string_view` or `std::vector<std::uint8_t>`, and optionally
the `ActivateOptions` of a user-locked key, passed on to a key file's activation), never its path,
and verifies it offline: signed by a key of `publicJwk`, for this product and, for an offline file, covering this
build's major. It answers an `ImportLicenseFileResult`: `ok` with the file's `kind` (`LicenseFileKind::Key` or
`Offline`) and `licensedTo`; or `error`, one of the `LicenseFileError`s `Malformed`, `NotALicenseFile`,
`BadSignature`, `UnknownKey`, `WrongProduct`, `RevokedFile`, `Upgrade`, `Outdated`, nothing stored; or, for a key
file whose key did not activate, `activateError`, what `activate()` answers. It throws `keygrant::StalledError` when a
licence call before it has not finished, and the storage's exception when the state cannot be read or the file cannot
be saved: try again. `UnknownKey` means the file was signed by a key this build does not carry: a newer one, after the
product rotated its key, which an update of the app carries. A key file's notes print its key, which can be typed
meanwhile.

An offline file is kept and checked again on every `status()`, which then answers `Licensed` with
`source == LicenseSource::File`, the key, `licensedTo` and the file's entitlements, never asking the server,
`refresh()` included. Show "Licensed to Ada Lovelace" from `licensedTo`. A status from a file has no seat to
deactivate: offer `removeLicenseFile()` instead (`deactivate()` leaves the file alone). If the device also holds a
key, a file that covers this build wins, and the key keeps working once the file is removed, revoked or outgrown. A
file that does not cover this build answers only when no key and no running trial does.

**Keep every key that signed a file in your builds.** A file names the key that signed it and never expires, so a key
that has signed one stays in the product's key set, and in `publicJwk`, after it stops signing leases. A build
without it reads such a file as `Invalid` / `UnknownKey`.

### User-locked keys

A licence model can let only the person a key was sold to activate a NEW device on it: the activation names the
address on the key and, for a key that mails a code to that address, the code too. Only a new device is ever asked;
`status()`, `refresh()`, a renewal, a seat the key already holds and everything offline are as they were, so an app
that is activated keeps running through a KeyGrant or mail outage.

`activate(key, options)` carries what the person typed (`keygrant::ActivateOptions`; each member is an optional
string, and neither is ever stored):

```cpp
keygrant::ActivateOptions options;
options.email = emailField.text();
options.code = codeField.text();
const keygrant::ActivateResult result = license.activate(key, options);
if (!result.ok) {
  switch (*result.error) {
    case keygrant::ActivateError::UserEmailRequired:
    case keygrant::ActivateError::UserEmailMismatch: askForTheAddress(); break;  // then activate again with it
    case keygrant::ActivateError::UserCodeRequired:
    case keygrant::ActivateError::UserCodeInvalid: askForTheCode(); break;       // a code was just mailed to it
    case keygrant::ActivateError::TooManyCodes:
    case keygrant::ActivateError::TooManyAttempts: askToTryLater(); break;
    case keygrant::ActivateError::TryAgain: askToRetry(); break;                  // nothing was activated
    default: showTheOtherErrors(); break;
  }
}
```

| `ActivateError` (its word) | Means | The app |
|---|---|---|
| `UserEmailRequired` (`user-email-required`) | the key is locked and no address came | ask for the address |
| `UserEmailMismatch` (`user-email-mismatch`) | not the key's address (nothing is said of the real one) | ask again |
| `UserCodeRequired` (`user-code-required`) | the address was right and a code was just mailed to it | ask for the code |
| `UserCodeInvalid` (`user-code-invalid`) | the code is wrong, expired (10 minutes) or used, or the key's codes are void for the hour after five wrong ones | ask again, or activate with the address alone for a new code |
| `TooManyCodes` (`too-many-codes`) | three codes were mailed for the key this hour | wait, or use the code sent |
| `TooManyAttempts` (`too-many-attempts`) | ten wrong addresses this hour, or the key's codes void | try again later |
| `TryAgain` (`try-again`) | the code could not be mailed: nothing was activated | retry; the one error here that is nobody's mistake |

The address is trimmed (it is compared without regard to case) and a blank is none. The code has its ends trimmed and
its spaces dropped (`123 456` is `123456`), and must be exactly six ASCII digits. An address longer than 254 UTF-16
units, or a code that is not six digits, answers `UserEmailMismatch` or `UserCodeInvalid` at once, for any key, asking
the server nothing and so costing the key none of its wrong tries; the key is judged first, then the address, then the
code. A key that is not locked ignores both once they reach the server. `importLicenseFile(content, options)` passes
them on to a key file's key. `deactivate()`, `refresh()` and `startTrial()` take neither.

### Metered uses: counting what a key may do

A licence model can count uses of what it sells: 1,000 exports for good, or 50 renders each billing period. The app
spends them with `use(meter, n)`, before the work they count, and does the work only when the spend is `ok`:

```cpp
const keygrant::UseResult spent = license.use("exports");   // use("exports", 40) spends forty at once, whole or none
if (spent.ok) {
  runExport();   // spent.left: what is left now, net of this device's unsynced spends
} else {
  switch (*spent.error) {
    // The server's word: fewer left than asked (spent.left says how many; nullopt when it gave no number).
    case keygrant::UseError::UsedUp: offerMoreExports(spent.left); break;
    // KeyGrant could not be reached, and this is past what this device may spend without it.
    case keygrant::UseError::OfflineLimit: askToConnect(); break;
    // NoMeter, NotLicensed, Revoked, Device, Upgrade, Outdated, Storage, InvalidAmount.
    default: showMeterProblem(*spent.error); break;
  }
}

const keygrant::LicenseStatus status = license.status();   // offline, from the lease
status.meters.at("exports");   // MeterStatus{left 950, total 1000, offline 100, unsynced 0, resets false}
```

Like every call it **blocks**, so make it off your UI thread (the Qt example above works the same way).

- **Online it is exact.** The server takes `n` only while the meter has room for all of it. Two calls are two spends;
  the SDK's own retries and reports never count one twice.
- **With no answer it spends from an allowance.** No network, a timeout, a 5xx, a 429 or an answer the SDK cannot use:
  the device spends from its offline allowance for that meter (set on the licence model; a tenth of the meter by
  default, 0 for online only), never past what it last heard is left, answers `offline == true`, and reports the spend
  with its next request. Past either, `OfflineLimit`. A spend the allowance would refuse asks the server first,
  whatever the last outage left behind, so a customer who is back online is not refused for it (once a minute while
  KeyGrant stays out of reach); one the allowance covers is not asked again for a minute after an outage, growing to an
  hour. The unsynced spends are kept with the licence (`StoredState::spends`), so they survive a restart and a second
  instance of the app, and a spend the SDK cannot save yet is kept in memory: the customer always gets the use they
  were told they had, and it is reported once it has been saved. A `status()` on a valid lease reports the journal
  too, once its own wait is over, so a device back online gives its allowance back.
- **It needs a key's lease** that licenses this build on this device (`NotLicensed` otherwise, without asking). A
  device trial and an offline licence file have no meters.
- **`deactivate()` clears the unsynced spends with the licence.** It sends them with the release (and the device's
  fingerprints, so the server holds them to the seat's device), but clears them locally even when KeyGrant could not be
  reached (`released` is then false), so what the server did not hear is lost: at most the offline allowance of each
  meter. Activating another seat drops the old seat's spends unreported the same way.
- **It never throws for an outcome**: every one is in the `UseResult`, behind a call that has not finished too (it
  asks as usual and saves nothing: an offline spend is kept in memory and saved by the next call that can), and a state that cannot be
  read just now, or an adapter that threw, is `Storage`. A `use` request waits at most 5 seconds for the server (less
  when `httpTimeout` is shorter). Calls on one `License` run one at a time, this one among them, and a call takes no
  cancellation: it runs to its bounded answer, so a spend that was made is the customer's.
- **`n` is a `std::int64_t`**: anything outside 1 to `keygrant::kMaxMeterUses` (1,000,000,000) is `InvalidAmount`,
  and a meter code that is not 1 to 32 UTF-16 units is `NoMeter`, both without asking anything. A code is matched
  exactly, case included, as an entitlement's is.
- **`status.meters`** is there on a `Licensed` status from a key's lease that names meters, and empty on every other
  status: `left` is net of this device's unsynced spends, `total` is what the key holds (for good, or this period),
  `offline` is this device's allowance, `unsynced` what it has spent and not reported, and `resets` is true for a
  meter that comes back each billing period.
- **A meter never takes the licence away.** Nothing reads one to decide whether the app is licensed: a meter that is
  used up refuses spends, never the app.
- An app built with an SDK from before meters licenses a metered key as any other and cannot spend.

### Restoring a licence without the key (App Store builds)

A build that may not take a typed licence key (an App Store build) restores a web buyer's licence by the address the
key was sold to. The product's **Key pickup** must be on. The app asks for a code, the customer types the code that
comes to that address, and the SDK activates the key that fits this build:

```cpp
// 1. Mail a code to the address the key was sold to (the answer is the same whether or not the address holds a key).
const keygrant::PickUpSentResult sent = license.pickUp(emailBox.text());

// 2. The customer types the code; restore.
const keygrant::RestoreResult restored = license.restoreByEmail(emailBox.text(), codeBox.text());
if (!restored.ok) {
  if (restored.pickupError == keygrant::PickupError::WrongCode) askAgain();
  if (restored.pickupError == keygrant::PickupError::CodeExpired) license.pickUp(emailBox.text());  // a new code
  if (restored.pickupError == keygrant::PickupError::TooManyAttempts) waitSeconds(restored.retryAfter);
  if (restored.noKey) showNoKeyUnderThatAddress();
  if (restored.error == keygrant::ActivateError::Upgrade) offerAnUpgrade();      // the keys are for an older major
  if (restored.error == keygrant::ActivateError::Outdated) askToUpdateTheApp();  // the keys start at a later major
}
```

`pickUp(email, code)` hands back the address's keys instead (`PickUpKeysResult::keys`: show them by `modelName`, never
the key) for an app that lets the customer choose; activate one with
`activate(picked.key, {address, picked.activationCode})`. `restoreByEmail` chooses for the customer, among the keys that
cover this build's major: the key this device already holds a seat on, else one with a free seat, else the first.

| `PickupError` | Means | The app |
|---|---|---|
| `InvalidEmail` | the address is blank or longer than any | ask again |
| `WrongCode` | not six digits, or not the address's live code | ask again |
| `CodeExpired` | no live code (expired after 10 minutes, used, burnt by its fifth wrong try, or never asked for) | `pickUp(email)` again |
| `TooManyAttempts` | thirty wrong codes for the address this hour, or too many asks or confirms from this network | wait `retryAfter` seconds |
| `PickupDisabled` | the product's Key pickup is off | offer another way (support) |
| `TryAgain` | a count could not be made | retry |
| `Network` | no answer, or an answer that is not a pickup's | retry |

A `RestoreResult` holds a refusal in one of three members: `pickupError` (the code was not taken), `noKey` (the address
holds no key of the product) or `error`, an `ActivateError`: `Upgrade` (the keys are for an older major than this build)
or `Outdated` (their lowest version is newer), activating nothing; then `activate`'s own errors (`Limit`, `Revoked`, ...) as
they come, and `Network` behind a call that has not finished. The address and the code are prepared as an activation's
are (trimmed, a code's spaces dropped) and refused at once, asking nothing, when the address is blank or the code is not
six digits, so a mistyped code costs the address none of its wrong tries. `retryAfter` is read from the server's answer.
The first confirm spends the code, so the keys it answered are kept in memory for 10 minutes (a code's life) by the
address and code as sent: a confirm or a restore with the same pair answers from them and asks nothing, so a restore
whose activation got no answer can be retried with the same code, and a restore after `pickUp(email, code)` needs no new
one. A restore that activates drops them. Nothing about a pickup is stored (not the address, the code, the keys or a
refusal): the two `pickUp` calls write nothing, never throw and run beside any call, and `restoreByEmail` stores only the
activation, as `activate` does, runs one at a time with it, and throws as it does when the state cannot be read or saved.
A pickup never takes a licence away. The rules are in the SDK contract's section on key pickup, which the conformance
vectors `pickupAsks`, `pickupRefusals`, `pickedKeys` and `pickupChoices` hold this SDK to.

### Buying from the app

Give a customer who holds no key a Buy button that opens your Stripe Payment Link. The checkout emails them their
key, and they type it into the app:

```cpp
// "Buy" on the trial or activation screen: QDesktopServices::openUrl, ShellExecuteW, NSWorkspace openURL, ...
openInBrowser("https://buy.stripe.com/your-link");

// Once they have the key from the email:
const keygrant::ActivateResult result = license.activate(enteredKey);
```

For a customer who already holds a key, `upgradeUrl(paymentLink)` adds this install's activation to the link, so an
upgrade sale finds the key it upgrades.

### Trials: what the app calls

```cpp
keygrant::LicenseStatus status = license.status();
if (status.state == keygrant::LicenseState::Unlicensed) status = license.startTrial();   // begins the trial, once
```

A running trial reports its launch to KeyGrant by itself: `status()` sends it at most once a day, on a thread of its
own, answering without waiting for the request and never failing offline (it saves the day's stamp first), so the
dashboard's trial funnel sees the trial come back.
The app does not need to call `startTrial()` on every launch for that. `startTrial()` still re-syncs the trial when
called: a change to the trial's length or entitlements reaches a running trial there.

## Configuration

| Field | Default | |
|---|---|---|
| `product` | (required) | Your product's slug. |
| `apiBaseUrl` | (required) | `https://api.keygrant.dev`. |
| `publicJwk` | (required) | Your product's public keys: the key set (`{"keys":[...]}`), a JSON list of JWKs, one JWK, or its bare `x`. A private key is refused. |
| `major` | `1` | This build's major version, at most 99999. Set it: a key's coverage is judged by it. Read as KeyGrant's server reads a version: a 0.x build (`major = 0`) is major 1, and so is anything below 1. |
| `deviceName` | none | Shown in the customer's device list; cut to 120 UTF-16 units. |
| `deviceHold` | `true` | Whether a licence is held to its device (below). |
| `httpTimeout` | 15 s | How long any request may take; the SDK holds every request to it itself. |
| `revokedLicenseFiles` | empty | The ids (`fid`) of offline licence files that leaked, which this build refuses: a stored one is `Invalid` / `RevokedFile` (below). |
| `adapters` | defaults | Storage, registry stash, clock, fingerprinter, HTTP (below). |

### Embedding the public keys

`publicJwk` is the product's key set: the `keys` of `GET https://api.keygrant.dev/v1/products/<slug>/pubkey`
(public, no API key) as `{"keys":[...]}` (or that whole answer), which the dashboard's Signing key tab also copies
(Copy key set). Fetch it when you build and ship it with the app, in your source or a resource file. One key alone,
its JWK or its bare `x`, still works: a set of one. The keys are public (anyone may verify a lease with them); never
embed a private key: the constructor refuses a JWK with `d`. A wrong key shows up at once: `activate()` answers
`InvalidLease`.

**Why a set.** A lease names the key that signed it (`kid` in its header, the key's RFC 7638 thumbprint, which the
SDK computes from the key itself). It is verified by that key; a lease that names none (every lease signed before key
sets) by each key in turn; and one naming a key the build does not carry is refused (`UnknownKey`). When the product
rotates its signing key, it first *stages* the next key, published beside the current one while every lease is still
signed with the current key. Ship builds carrying both; once they are out, the product *switches*, and those builds
verify every lease signed after it. Embed the whole set each time you build. Each activation and validate
reports the ids of the keys the build carries (`kids`), so the dashboard can count the devices still on builds
without the staged key before you switch.

### deviceHold

A lease names the device it was activated on (a hash of the OS machine id: Windows `MachineGuid`, macOS
`IOPlatformUUID`, Linux `/etc/machine-id`), so a state file copied to another machine does not license it.
A machine whose id cannot be read uses its host name, which is never held. Set `deviceHold = false` for a product
on non-persistent, pooled VDI, where every session may be another machine: no lease is then judged by its device,
and the server is sent no fingerprints. The device limit still counts seats.

### Test licences

A test licence (made from the dashboard, a `kg_test_` API key, or a Stripe test-mode checkout) is signed with your
real key and licenses the app like any other, for at most 30 days. `status.test` says so; a release build can refuse it.

### Entitlements

What a licence grants beyond the base product (an edition, a number of seats, an add-on) is signed into its lease
(the `ent` claim), and a status hands it to the app as `status.entitlements`, a
`std::map<std::string, keygrant::Entitlement>`:

```cpp
const keygrant::LicenseStatus status = license.status();
const auto edition = status.entitlements.find("edition");
const bool pro = edition != status.entitlements.end() && edition->second == "pro";
```

- **Present** on a `Licensed` status, from the licence's lease (read offline, or the lease an activation, a renewal or
  `refresh()` brought), and on a `Trial` status, from the trial's lease; a `stalled` answer carries them too, from the
  lease stored. **Empty** on every other status (`Expired`, `Invalid` for any reason, a stored refusal included,
  `Unknown`, `Unlicensed`), and when the lease grants none.
- A `keygrant::Entitlement` is exactly one of a flag, a number or a text. A number is a `double`, so 5 and 5.0 are the
  same entitlement. `kind()`, `isFlag()`, `isNumber()` and `isText()` say which; `flag()`, `number()` and `text()`
  read it (`std::nullopt` for another kind). It compares by kind and value, a literal included (`== "pro"`, `== 5`,
  `== true`). A narrow string literal (`"pro"`) or C string makes a text on every compiler, and an int a number,
  where a bare `std::variant<bool, double, std::string>` may make a bool of the one (C++17's rule) and refuses the
  other. What would otherwise be read as the flag `true` or a number does not compile, as an entitlement or compared
  with one: a wide, UTF-16, UTF-32 or C++20 `u8` literal (`L"pro"`, `TEXT("pro")` on a UNICODE build, `u"pro"`,
  `U"pro"`, `u8"pro"`: compare with a narrow UTF-8 one), a character (`'p'`), an enum or a null pointer.
- A member of `ent` that is not a flag, a number or a text (an object, a list, `null`) is dropped, and an `ent` that is
  not an object reads as none. Neither ever costs the licence, or the entitlements beside it.
- No caps are enforced (how many there are, how long a name or a text is): the lease's signature already vouches for
  what the server signed.
- The map is the status's own copy: changing it changes nothing the next status says.

To ask whether the licence grants an entitlement, name its code:

```cpp
if (license.has("cloud_sync")) enableCloudSync();
license.require("export");  // throws keygrant::MissingEntitlement (entitlement() is "export")
```

`has(code)` is true for a flag that is on, a number that is not zero, or a text that is not empty; false for anything
else, and for a code the lease does not grant (matched exactly, byte for byte). It is answered offline from the stored
lease only, as `status()` would answer from what is stored, with no request and no write: a licence that does not
license this build, or none at all, grants nothing; a running trial grants what its trial lease does. It never
throws. `require(code)` throws `keygrant::MissingEntitlement`, naming the code, when `has` would be false.
`keygrant::grants(status.entitlements, code)` answers the same of a status already in hand.

## Where the state is kept

One JSON file per product, per user, on this machine (never the working directory, never roaming AppData):

- Windows: `%LOCALAPPDATA%\KeyGrant\<product>.json`
- macOS: `~/Library/Application Support/KeyGrant/<product>.json`
- Linux: `$XDG_CONFIG_HOME/keygrant/<product>.json`, else `~/.config/keygrant/<product>.json`

With no home folder to find (`HOME` or `USERPROFILE` unset, empty or relative, and nothing absolute in its
place: an arbitrary UID in a container, say), the `License` constructor throws `std::invalid_argument` rather
than keep the state in the working directory; give it `config.adapters.storage` with a path of your own.

The format is the reference's, field for field, so every KeyGrant SDK reads the others' files. It is written
atomically (a temp file flushed to the disk, renamed over the file, busy renames retried, the folder flushed on
POSIX), owner-readable only on POSIX (0600), stamped with a revision so two processes never lose each other's
saves, and read so that only a missing file is "no state": a file that cannot be read just now is `Unknown`, one
that does not parse is set aside as `<file>.<ms>.corrupt` (the newest three kept).

`keygrant::FileStorage(path, options)` puts it elsewhere; `options.migrateFrom` copies a state you kept somewhere
before, once. `keygrant::serializeState`/`parseState` give the format to a storage adapter of your own. The journal of unsynced
[metered](#metered-uses-counting-what-a-key-may-do) spends (`spends`) and the newest figures a spend was answered with
(`meterFigures`) are fields of it: a storage that keeps the whole state keeps them, one that drops a field loses the
report of what that field held.

## Threading

- Calls on one `License` run **one at a time**, from any number of threads.
- A call waits for the one before it, but not past that call's HTTP bound plus 15 s from when it started. Past
  that (an adapter stuck on a dead disk, say), it runs **read-only**: it answers from what is stored, asks nothing,
  writes nothing, and says `stalled`. `activate()` read-only is `Network`; `deactivate()`, `importLicenseFile()` and
  `removeLicenseFile()` throw `StalledError`.
- Every request, fingerprint and machine-id read is held to its bound by running it on a thread of its own, which
  is **abandoned** (left to finish, its answer dropped) when it outlives the bound. An HTTP adapter that ignores
  its timeout cannot hold a call open.
- The fingerprinter and the machine id are read **one at a time**: a call that comes while a read is running waits
  on that read (for its own bound) instead of starting another, so a read that never ends (a hung filesystem, a
  filter driver, a custom fingerprinter that hangs) costs one thread, not one more on every `status()`.
- Destroying a `License` while a call is stuck in an adapter is safe: that call finishes on its own.
- A running trial's launch report (below) is sent on a **thread of its own**, which the `status()` that started it
  never waits for. That thread holds what it uses (the device reader with its fingerprinter, and the HTTP adapter,
  each by `shared_ptr`), never the `License` itself, until the report settles, within about 8 s (up to 3 s for the
  device: its 2 s and the 1 s an `identify()` is given past it; 5 s for the request), so destroying the `License`
  first is safe; the fingerprinter and the HTTP adapter are then called from that thread too.
- Adapters may therefore be called from several threads at once, and must be thread-safe. The defaults are.
- An abandoned adapter call, or a launch report in flight, may still be running when the process exits, so a custom
  adapter should keep what it uses in its own object (the SDK holds adapters by `shared_ptr` for as long as a call
  runs), not in statics that are destroyed at exit.

## Adapters

All platform I/O is behind an interface; leave any `config.adapters` field empty for the default.

| Adapter | Default | |
|---|---|---|
| `StorageAdapter` | `FileStorage` at the path above | `read(readOnly)`, `write(state)`, optional `update(merge)`. |
| `RegistryStash` | `NoopRegistryStash` | Keeps a trial's start where an uninstall leaves it. Opt in on Windows with `WindowsRegistryStash(product)` (below). |
| `Clock` | `SystemClock` | Milliseconds since the epoch. |
| `Fingerprinter` | `SystemFingerprinter` | The OS machine id (no subprocess: the registry API, IOKit, the files), else the host name. Implement `identify()` and report `DeviceKind::Machine` for a stable id that should hold a seat; `get()` alone is never held. |
| `HttpAdapter` | `CurlHttp` | One JSON POST; return the status and body whatever the status, throw when there is no answer. |

**`WindowsRegistryStash(product)`** keeps the start in the one format every KeyGrant SDK shares, so a trial one SDK
stashed is seen by another for the same product: `HKCU\Software\KeyGrant\<product>`, value `TrialStart`, the start
in milliseconds since the epoch as a `REG_QWORD`. It reads a `REG_QWORD`, or a `REG_SZ` of decimal digits (what a
script or a hand writes); anything else is no start (another type, 0, or 2^53 or more). Best-effort: a read or
write that fails costs nothing.

**`CurlHttp`** verifies TLS against the system's CAs (`CurlHttpOptions::caInfo` for a bundle of your own): an
answer over a connection it cannot verify is no answer, since a refusal is unsigned JSON. It does not check
revocation (`CURLSSLOPT_NO_REVOKE`), as fetch does not: on Windows, Schannel would otherwise refuse any
certificate whose revocation status it cannot find, so a network that lets the API through but not the CA's CRL
hosts, or a TLS-inspecting proxy, would answer no activation or renewal at all. It follows a 307 or a
308 as fetch does, with the method and the body, to `https` only and at most 20 times; any other 3xx is the answer
as it is. It caps a body at 1 MiB and sends a `User-Agent`. libcurl's global set-up runs once, when the first
`CurlHttp` is made (the `License` constructor makes the default one), on that thread. **With libcurl older than
7.84** (Ubuntu 22.04 has 7.81) that set-up is not thread-safe: call `curl_global_init(CURL_GLOBAL_DEFAULT)` in
`main()` before your app starts any thread. CMake warns when it finds such a libcurl.

## Building and testing it

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

As the top-level project the SDK builds its tests (GoogleTest, besides its own dependencies; with
vcpkg, the manifest's `tests` feature: `-DVCPKG_MANIFEST_FEATURES=tests`) and treats warnings as
errors; inside your project it does neither. The tests read `conformance/vectors.json`
(`KEYGRANT_VECTORS` overrides the path).

The suite: every conformance group replayed as the reference answers it (all but the `majors` fractions, which only the
reference's JavaScript number can hold), each from the pure function the engine itself calls; the engine against fake
adapters (a fake clock, in-memory storage, scripted HTTP, a fixed fingerprinter), with server bodies nested 400,000
deep; the file storage against an in-memory file system that fails, holds and interleaves on demand, and against the
real one; the machine id's parsing for every platform, and this machine's own id against what `reg query` (Windows) or
`/etc/machine-id` (Linux) says; the Windows stash against the real registry, under a root of the tests' own; and libcurl
against servers on 127.0.0.1, plain and TLS (a certificate no system trusts, CA-issued leaves whose revocation cannot be
checked, redirects), on Linux and Windows alike.

Key pickup has its own: the four pickup groups of the conformance vectors, the requests, every refusal, nothing written by
`pickUp`, a restore's activation with the address and the activation code, `noKey`, `Upgrade` and `Outdated`, the keys kept
in memory, and a pickup behind a stuck call.

Metered uses have their own: the twelve metered groups of the conformance vectors (all but the two `useAmounts` that
give `use` a fraction and a text), the pure decisions they pin, `use()` online and with no answer, the waits, what a
request asks and reports, a spend held in memory after a failed save, two processes over one state file, and the
journal's limits and its horizon. A property test (`tests/use_property_test.cpp`) runs 1,500 seeded sequences of
spends, check-ins, releases and restarts from two processes over one state file, against a server that loses, delays
and refuses answers (or loses them after recording the spend), a disk that refuses its writes and a clock that jumps,
and holds the contract's invariants at every step and at the end. `PROPERTY_RUNS` and `PROPERTY_SEED` choose how many
sequences run and from which seed; a case it finds is shrunk and printed as data, to be kept with the cases beside it
(`tests/property/cases.json`).

## Parity with the reference

Where this SDK differs from the Electron SDK, and why:

- **Key pickup** is `pickUp(email)`, `pickUp(email, code)` and `restoreByEmail(email, code)` as overloads, answering a
  `PickUpSentResult`, a `PickUpKeysResult` and a `RestoreResult` (`ok`, `pickupError`, `noKey`, `error`, `retryAfter`),
  where the reference resolves one `{ ok, error?, retryAfter? }` whose `error` holds any of the words; `restoreByEmail`
  throws what `activate` throws (the storage's exception) where the reference rejects.
- **Bad configuration throws at construction** (`std::invalid_argument`): a JWK that is not an Ed25519 public key
  (a private one included), an empty or path-like `product`, an `apiBaseUrl` that is not http(s), a `major` above
  99999 (the server refuses one in every request), a non-positive `httpTimeout`. The reference takes
  anything and fails every call later. The JWK's `x` is canonical base64url; trailing padding is allowed.
- **The machine id is read through the OS API, not a command**: `RegGetValueW` (`RRF_SUBKEY_WOW6464KEY`, falling
  back to `RegOpenKeyExW` with `KEY_WOW64_64KEY`) instead of `reg.exe`; IOKit (`IOServiceGetMatchingService`,
  `IORegistryEntryCreateCFProperty`) instead of `ioreg`. The value is taken as the reference takes it out of their
  output (the first token; up to a quote, trimmed), so a machine has the same fingerprint whichever SDK reads it.
  A value that is not there, not readable or of another type is `unavailable`; any other failure is `transient`.
  The read is bounded as a whole (its timeout plus 500 ms), not file by file.
- **Text is JavaScript's**: key trimming uses ECMAScript's white space; upper-casing uses the full Unicode mapping
  (`src/upper_table.inc`, generated from Node 22's `toUpperCase` by `scripts/gen-uppercase.mjs`); lengths are UTF-16
  units. Ill-formed UTF-8 from the app (a key, a device name, a custom fingerprint) or the system (the host name,
  as Node's `os.hostname()` reads it) is read as JavaScript would hold it, each bad sequence one U+FFFD, so the
  server, the hashes and the file all see the same string.
- **Numbers are 64-bit integers**, held to JavaScript's safe range (±(2^53-1)); a lease claim or stored time beyond
  it is saturated, which keeps every comparison with a real time or major as JavaScript makes it.
- **A lease or licence-file payload nlohmann/json will not parse is refused whole**: one holding an escaped lone
  surrogate (`"\ud800"`) is `bad-claims` here, where the reference's `JSON.parse` reads it as a string and keeps the
  entitlement. The server never signs one: its signer refuses an entitlement name or text with a lone surrogate, and
  every other claim is one it writes itself.
- **A number past a double's range** (`1e400`, which `JSON.parse` reads as `Infinity`) fails nothing: wherever the SDK
  reads JSON from a lease, a licence file or a server's answer, the member it is the value of is read as absent, as a
  member of another shape is. A meter holding one is dropped, as the reference drops one whose figure is not a whole
  number, and the lease stands; an entitlement holding one is dropped too, where the reference keeps `Infinity`. The
  server never signs an entitlement number that is not finite.
- **The state file**: a leading byte-order mark is accepted (the reference sets such a file aside as corrupt); a known
  field of the wrong JSON type, or with a value this SDK does not know (a newer SDK's refusal or device kind), is read
  as absent; JSON that is not an object is no state. Fields this SDK does not know, and those values it cannot read, are
  kept and written back as they were, as the reference keeps them, until this SDK writes or clears the field itself.
  There is no default migration from `<cwd>/.keygrant/`, the legacy location older Electron SDK versions used
  (`FileStorageOptions::migrateFrom` exists).
- **Windows file replacement** is `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`, the call Node's
  `rename` makes, rather than `ReplaceFileW`, which fails when the target does not exist yet. Win32 errors are named as
  libuv names them (a sharing violation is `EBUSY`), so the retry rules read the same.
- **POSIX permissions**: the state file is created 0600 and its folders 0700 (the reference: the umask's default).
- **HTTP**: a 307 or 308 is followed as fetch follows it (the method and the body kept, at most 20), but only to
  `https`; fetch would also follow one to plain http, and a 301, 302 or 303 as a GET, which this SDK answers as
  the 3xx it is. A body is capped at 1 MiB, and a `User-Agent` is sent. Certificate revocation is not checked, as
  fetch does not check it (`CURLSSLOPT_NO_REVOKE`; it only matters to Schannel, on Windows, which would otherwise
  refuse whatever certificate its revocation status could not be found for); the other KeyGrant SDKs' default
  HTTP stacks do not fail on it either.
- **A server's body or a state file nested deep** is never copied or written recursively past 64 levels: a body
  is moved, never copied; a value in the state file this SDK cannot read is kept only while it nests 64 deep or
  less (an unknown field nested deeper is dropped at the next save, rather than overflow the stack).
- **Bounds are threads**: where the reference races a promise against a timer, this SDK runs the adapter call on a
  detached thread and stops waiting for it; the abandoned call finishes on its own. The launch report, a promise the
  reference never awaits, is a detached thread here too. A fingerprinter or machine-id
  read still running is waited on by the next call, not started again beside it (a promise costs nothing to
  abandon; a thread does).
- **Never throws from `status()`/`refresh()`/`startTrial()`** for anything, not only storage: an unexpected
  exception (a custom clock that throws, say) is answered `Unknown`/`Storage`. The reference would reject.
- **`WindowsRegistryStash`** is an opt-in default the reference does not ship (Electron apps inject their own); it
  keeps the one stash format every KeyGrant SDK shares.
- **`upgradeUrl`** reads the link with a small URL routine, not a full WHATWG parser: the scheme and
  an http(s) host are lower-cased, and the query and fragment get the percent-encoding the WHATWG parser gives them;
  a default port, a path and an international host are left as given (the parser would drop the port, normalize the
  path and write the host in Punycode).
- `LicenseStatus::test` and `stalled` are `bool` (false where the reference leaves them out); `trialDaysLeft` and
  `trialEndsAt` are `std::optional`; `entitlements` is a `std::map`, empty where the reference leaves it out, and the
  status's own copy where the reference hands out a frozen one.
- **Licence files**: `importLicenseFile` answers a refusal (`error`) and a key file's failed activation
  (`activateError`) in two members, where the reference's one `error` holds either word, and throws
  `StalledError` behind an unfinished call where the reference rejects. A stored `licenseFile` that is not text is read
  as no file, as every known field of the wrong type is (the reference reads it as `Invalid`/`Tampered`). A whole
  number in a file's claims past the safe range is held at its end, as a lease claim is.
- **User-locked keys**: `activate(key, options)` and `importLicenseFile(content, options)` are overloads with an
  `ActivateOptions` struct, where the reference takes an optional second argument; a code is text here, where the
  reference also reads a code kept as a number.
- **Metered uses**: `use(meter, n)` has a default `n` of 1 and answers a `UseResult` struct (`ok`, `left`, `offline`,
  `error`) where the reference answers one object: `left` is a `std::optional<std::int64_t>` (nullopt where the reference
  gives none), `error` a `std::optional<UseError>`. `n` is a `std::int64_t`, so a fraction and a text cannot be asked:
  the two `useAmounts` vectors that give one are skipped. The call blocks and takes no cancellation: it runs to its
  bounded answer, as every call of this SDK does. `StoredState::spends` and `meterFigures` are typed lists and maps, read
  leniently as the reference reads them: a journal that is not a list, an entry or a figure of another shape, is read as
  empty or left out, and so are the unknown members of a valid entry, so any save writes the state without them (the
  reference keeps them until a journal change lands). A spend's id is 16 bytes from OpenSSL's random source
  (`RAND_bytes`), which the SDK links already.
- The SDK's fixed timings exist as `keygrant::detail::Timings` so the tests can run them shorter; it is not a stable API.
- The conformance file's Windows and macOS `machineIds` cases script `reg.exe` and `ioreg`; the suite replays them
  through the API reads this SDK makes (the printed value is the value the API returns; a command that said no, or
  is not there, is a value that is not there; a command killed at its time box is a read that does not finish).
