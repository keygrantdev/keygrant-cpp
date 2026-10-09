# KeyGrant for C++

Licensing for native desktop apps (Qt, wxWidgets, JUCE, plain Win32, Cocoa or GTK): a licence is proven to the app by a
signed lease (a JWT, EdDSA over Ed25519) that the SDK verifies **offline** against your product's public key. The app
only talks to KeyGrant to activate, to renew a lease near or past its end, to report a newer major version, to pick up a
key bought through `purchaseUrl()`, or to report a running trial's launch (at most once a day).

The rule every line of it serves: **licensing failure fails in favour of the
paying customer.** An outage, no network, a timeout, a 5xx, a 429, an ambiguous
4xx, a body it cannot use, or a disk it cannot read just now never takes a
licence away while the device holds a valid lease. Only an explicit verdict
from the server (revoked, upgrade or update required, another device) or the
lease's own end does.

This is a port of the Electron SDK (`@keygrant/electron`, the reference), held to the same behaviour by every answer in
`conformance/vectors.json` (every vector is replayed by the suite but the `majors` fractions, which no `std::int64_t`
major can hold). The full guide is at https://keygrant.dev/docs.

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
  GIT_TAG        0.3.0)
FetchContent_MakeAvailable(keygrant)

target_link_libraries(my_app PRIVATE keygrant::keygrant)
```

or, with the repository vendored, `add_subdirectory(third_party/keygrant)`. Built inside your project the
SDK builds no tests and never adds `-Werror` to your build.

OpenSSL 3, libcurl and nlohmann/json must be findable by `find_package`. Installed (`cmake --install`), the SDK
is a package: `find_package(keygrant 0.3 REQUIRED)` (a static install finds OpenSSL and CURL again for you).

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
// A QObject living on the UI thread; the call runs on Qt's thread pool.
QFuture<keygrant::LicenseStatus> future = QtConcurrent::run([this] { return license_->status(); });
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
| `Licensed` | | Run. `test` is true on a test licence: refuse it in a release build if you want to. `entitlements` is what it grants (below). |
| `Trial` | | Run. `trialEndsAt` (ms) is the end the trial's lease was signed to, and `trialDaysLeft` counts down to it. `entitlements` is what the trial's lease grants. |
| `Expired` | `Offline` | The lease ran out and the server could not be reached: show "connect to renew". |
| `Expired` | `Trial` | The trial is over: its signed trial lease is no longer valid. |
| `Invalid` | `Revoked` | The key was revoked, expired, or this device released. |
| `Invalid` | `Upgrade` | This build is newer than the key covers: buy an upgrade (`upgradeUrl`). |
| `Invalid` | `Outdated` | This build is older than the key's lowest version: update the app. |
| `Invalid` | `Device` | The licence was activated on another device: activate on this one. |
| `Invalid` | `Tampered` | The stored lease is not this licence (another key, product or signer). |
| `Invalid` | `UnknownKey` | The stored lease is signed by a key this build does not carry. Online, the server renews it with the key it signs with now; when it stays so online, update the app. |
| `Unknown` | `Storage` | The licence file could not be read just now: keep running, ask again. |
| `Unlicensed` | | Nothing activated, no trial. |

`stalled` is true when the answer came from storage behind a call that has not finished (below).

`activate()` errors: `InvalidKey`, `Limit` (no seat left), `Revoked`, `Upgrade`, `Outdated`, `Network`
(no answer, or one it could not use: try again), `InvalidLease` (the server's lease did not verify here: a wrong
`publicJwk` or `product`). `NotConfigured` is named for parity and never returned.

### The calls

- `activate(key)`: the key is trimmed and upper-cased first; empty or over 120 characters is `InvalidKey` without
  asking. Throws when the stored state cannot be read or saved (the storage's exception, e.g. `keygrant::IoError`).
- `status(forceOnline = false)`, `refresh()` (= `status(true)`): never throw. A trial whose lease does not verify
  while its stored end is still ahead (a lease signed before the product's key was rotated, say) is asked for again,
  under the usual waits and bounded as a status check is; its answer stands, and an ask that brings back no trial
  this build can verify waits a minute (no answer) or an hour (an answer) before the next.
- `deactivate()`: releases the seat and drops everything but the trial evidence and the clock guard, answering
  `DeactivateResult{released}`: `true` when KeyGrant answered the release with a 200, or when no key and activation
  were held; `false` when it could not be asked or answered anything else (a 5xx, a 429, a proxy's page). Then the
  seat may still be held: say so (the developer's dashboard or support can release it), rather than tell the
  customer it is free. The local licence is dropped either way. An activation that is another device's (a state
  copied or restored from another PC: its lease, valid or run out, held to a machine this run can tell is not this
  one, or a device refusal that stands for this one) is dropped here without asking KeyGrant to release it, which
  would lock that PC out; `released` is then `true`, as this device held no seat. Throws `keygrant::StalledError`
  behind a stuck call (nothing released or dropped), and the storage's exception.
- `startTrial()`: never throws. The device is read once for everything it asks. With no key held and a purchase secret
  held (`purchaseUrl`, below), the key bought with it is asked for first, always, bounded by 5 s: bought, it is the
  answer (`Licensed`) and no trial is asked. A trial runs only while its
  trial lease (signed by KeyGrant to the trial's own end)
  is valid: the end stored beside it is unsigned, and editing it extends nothing. An answer whose trial lease this
  build cannot verify (signed with a key the product rotated to ahead of this build, say) never replaces a stored one
  it can, which would end a running trial; an answer with no lease (the trial is over) is taken as it is. A trial the
  disk refused is kept for this process and saved by the next call that can.
- `upgradeUrl(paymentLink)`: your Stripe payment link with `client_reference_id` set to this device's
  **activation id** (never the key), or `std::nullopt` with no activation. An upgrade sale finds the key through it.
- `purchaseUrl(paymentLink)`: your Stripe payment link with `client_reference_id` set to the reference to this
  install's one-time purchase secret, or `std::nullopt` while any key is held (below).

Both links are built one way: the link's own parameters are kept as written and in their order, any
`client_reference_id` already on it is replaced, and the fragment is kept. Both throw `std::invalid_argument` for a
link that is not an absolute URL.

### Buying from the app: no key to type

Give a customer who holds no key a Buy button that opens your Stripe Payment Link through `purchaseUrl`. When they
come back to the app, the key they bought is picked up and this device activated on it, with nothing typed:

```cpp
bool checkoutOpened = false;
if (const std::optional<std::string> url = license.purchaseUrl("https://buy.stripe.com/your-link")) {
  openInBrowser(*url);   // QDesktopServices::openUrl, ShellExecuteW, NSWorkspace openURL, ...
  checkoutOpened = true; // url is std::nullopt while a key is held: use upgradeUrl() for an upgrade
}

// When the customer is back (the window regains focus, say), off the UI thread, and only while a
// checkout is open: refresh() on a licensed install validates online every time.
if (checkoutOpened && license.status().state != keygrant::LicenseState::Licensed) {
  const keygrant::LicenseStatus status = license.refresh();   // Licensed once the checkout has gone through
  if (status.state == keygrant::LicenseState::Licensed) checkoutOpened = false;
}
```

- **How:** `purchaseUrl` keeps a one-time secret for this install (256 random bits from OpenSSL's CSPRNG, never the
  device's fingerprint) and sets the link's `client_reference_id` to a hash of it, so a link that leaks (browser
  history, a screenshot) claims nothing. The checkout keeps that reference on the key it mints, and the SDK asks
  for the key with the secret: at once on `refresh()` or `startTrial()`, and from `status()` at most every five
  minutes for a day after the link, bounded by 5 s (after that, `refresh()` or `startTrial()` still ask). `status()`
  and `refresh()` ask only under this machine's own fingerprint; `startTrial()` falls back to the host name as an
  activation does. Until the key is bought, the status is what it was (the trial, or `Unlicensed`).
- **Turn the product's "Converts in place" switch on** once your app uses `purchaseUrl`: it is off by default, and
  until it is on every pickup is refused and the customer types the key from the delivery email. Know the trade
  before you do: whoever opens a checkout owns its pickup. Your Payment Link is public, so anyone can add a
  `client_reference_id` of their own to it and get buyers to pay through their copy of the link; the key bought that
  way is picked up by the link's maker, not the buyer. A link from the app is the buyer's own; a leaked one claims
  nothing, as it carries only a hash of the app's secret. A planted pickup shows in the key's history, and you can
  free the seat for the buyer.
- **Calling it again** hands out the same secret's link, so the key bought through either link is picked up (paying
  twice buys two keys; the second comes by email). A secret is dropped 30 days after the last link, once its key is
  picked up, or when the server answers that it never will be (the product's "Converts in place" switch is off, or
  another device took it): the customer then types the key from the delivery email. An activation and a
  deactivation drop it too. A drop is this device's alone: it revokes nothing on the server.
- **It throws** `std::invalid_argument` for a link that is not an absolute URL (before anything is read), the
  storage's exception when the state cannot be read or the secret cannot be saved (a link whose secret is lost could
  never be picked up), and `std::runtime_error` when the CSPRNG cannot give a new secret. Behind a call that has not finished it hands out a held secret's link as it is, and throws
  `keygrant::StalledError` when none is held.

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
verify every lease signed after it. Embed the whole set each time you build. Each activation, validate and claim
reports the ids of the keys the build carries (`kids`), so the dashboard can count the devices still on builds
without the staged key before you switch.

### deviceHold

A lease names the device it was activated on (a hash of the OS machine id: Windows `MachineGuid`, macOS
`IOPlatformUUID`, Linux `/etc/machine-id`), so a licence file copied to another machine does not license it.
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
before, once. `keygrant::serializeState`/`parseState` give the format to a storage adapter of your own.

## Threading

- Calls on one `License` run **one at a time**, from any number of threads.
- A call waits for the one before it, but not past that call's HTTP bound plus 15 s from when it started (5 s more
  after a `startTrial()`, which may ask twice: the purchase claim, then the trial). Past
  that (an adapter stuck on a dead disk, say), it runs **read-only**: it answers from what is stored, asks nothing,
  writes nothing, and says `stalled`. `activate()` read-only is `Network`; `deactivate()` throws `StalledError`.
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

## Parity with the reference

Where this SDK differs from the Electron SDK, and why:

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
- **A lease payload nlohmann/json will not parse is refused whole**: one holding an escaped lone surrogate
  (`"\ud800"`) or a number past a double's range (`1e400`) is `bad-claims` here, where the reference's `JSON.parse`
  reads the first as a string and the second as `Infinity`, and keeps such an entitlement. The server never signs
  either: its signer refuses an entitlement name or text with a lone surrogate, and an entitlement number that is not
  finite, and every other claim is one it writes itself.
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
- **`upgradeUrl` and `purchaseUrl`** read the link with a small URL routine, not a full WHATWG parser: the scheme and
  an http(s) host are lower-cased, and the query and fragment get the percent-encoding the WHATWG parser gives them;
  a default port, a path and an international host are left as given (the parser would drop the port, normalize the
  path and write the host in Punycode).
- `LicenseStatus::test` and `stalled` are `bool` (false where the reference leaves them out); `trialDaysLeft` and
  `trialEndsAt` are `std::optional`; `entitlements` is a `std::map`, empty where the reference leaves it out, and the
  status's own copy where the reference hands out a frozen one.
- The SDK's fixed timings exist as `keygrant::detail::Timings` so the tests can run them shorter; it is not a stable API.
- The conformance file's Windows and macOS `machineIds` cases script `reg.exe` and `ioreg`; the suite replays them
  through the API reads this SDK makes (the printed value is the value the API returns; a command that said no, or
  is not there, is a value that is not there; a command killed at its time box is a read that does not finish).
