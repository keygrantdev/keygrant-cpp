// What the tests share: a signing key, leases as the reference signs them, the conformance vectors,
// and fake adapters.
#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "keygrant/keygrant.hpp"

struct evp_pkey_st;

namespace kgtest {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;
using keygrant::ActivateError;
using keygrant::ActivateResult;
using keygrant::DeviceKind;
using keygrant::LicenseReason;
using keygrant::LicenseState;
using keygrant::LicenseStatus;
using keygrant::StoredState;
using Millis = std::chrono::milliseconds;

/**
 * Waits for a call left running on another thread (a call stuck on its read, let go just before) to finish,
 * so the harness it uses outlives it. What it answers is not what the tests using this check: each asserts
 * what the License did while that call was stuck. So its result is dropped, on purpose.
 */
template <typename T>
void finish(std::future<T>& running) {
  (void)running.get();
}

inline constexpr std::int64_t NOW = 1'757'000'000'000;
inline constexpr std::int64_t DAY = 86'400'000;
inline constexpr const char* PRODUCT = "sluice";
inline constexpr const char* KEY = "SLUICE-AAAA-BBBB-CCCC-DDDD";

// --- statuses, as the reference's tests write them -----------------------------

LicenseStatus status(LicenseState state);
LicenseStatus status(LicenseState state, LicenseReason reason);
LicenseStatus status(LicenseState state, LicenseReason reason, const std::string& key);
LicenseStatus licensed(const std::string& key, bool test = false);
LicenseStatus stalled(LicenseStatus status);
ActivateResult ok();
ActivateResult failed(ActivateError error);

// --- signing ---------------------------------------------------------------------

/** An Ed25519 key pair: a fresh one per test, or the conformance vectors' TEST key. */
class TestKey {
 public:
  TestKey();
  /// From a private JWK's `d`.
  static TestKey fromPrivate(const std::string& d);
  std::string publicJwk() const;
  /// The id a lease names this key by (its thumbprint), as a check-in reports it.
  std::string kid() const;
  /// The ids a check-in of a build carrying only this key reports (`kids`).
  json kids() const;
  /// A compact JWS over the payload's JSON, header {"alg":"EdDSA","typ":"JWT"}.
  std::string sign(const ordered_json& payload) const;
  /// A compact JWS over these exact payload bytes.
  std::string signRaw(const std::string& payload) const;
  /// A compact JWS over the payload's JSON, its header naming the key `kid`.
  std::string signNamed(const ordered_json& payload, const std::string& kid) const;
  /// A compact JWS over this exact signing input (`header.payload`).
  std::string signInput(const std::string& input) const;

 private:
  std::shared_ptr<evp_pkey_st> key_;
};

/** A lease's input, as the server's signer takes it. */
struct LeaseSpec {
  std::string sub = KEY;
  std::string product = PRODUCT;
  std::string aid = "act_1";
  std::string model = "perpetual";
  std::int64_t lim = 3;
  std::int64_t major = 1;
  std::int64_t ttlSeconds = 30 * 24 * 3600;
  std::int64_t now = NOW;
  /// Claims besides: fp, fpk, minMajor, upgradeUntil, test, grace...
  ordered_json extra = ordered_json::object();
};

/** The claims a spec signs: iat = floor(now / 1000), exp = iat + ttl. */
ordered_json claimsOf(const LeaseSpec& spec);

/// The `fp` claim of a fingerprint.
std::string claimOf(const std::string& fingerprint);

// --- the conformance vectors -------------------------------------------------------

/** conformance/vectors.json (KEYGRANT_VECTORS overrides the path). */
const json& vectors();

/** A StoredState from a vector's JSON object. */
StoredState stateFrom(const json& object);

// --- fakes ---------------------------------------------------------------------------

/** Something a fake waits on until a test lets it go. */
class Gate {
 public:
  void open();
  void wait();
  /// Whether something is waiting on it (or has).
  bool reached() const;
  bool waitReached(Millis timeout);

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool open_ = false;
  bool reached_ = false;
};

/** Storage in memory, which can be made to hang a read, hold a write, or fail reads or writes. */
class MemoryStorage final : public keygrant::StorageAdapter {
 public:
  std::optional<StoredState> read(bool readOnly) override;
  void write(const StoredState& state) override;

  std::optional<StoredState> get();
  void set(std::optional<StoredState> state);
  int writes() const { return writes_.load(); }
  /// The next read never settles until released (an adapter stuck on a dead disk).
  void hangNextRead();
  /// The next write is held inside the adapter with the state it was handed, landing when released.
  void holdNextWrite();
  Gate& readGate() { return readGate_; }
  Gate& writeGate() { return writeGate_; }
  std::atomic<bool> failWrites{false};
  std::atomic<bool> failReads{false};
  /// The next read fails, and only that one (a file an antivirus scan holds for a moment).
  std::atomic<bool> failNextRead{false};
  /**
   * Another process's save between two reads: the `nth` read from now (1 is
   * the next) first finds `state` stored.
   */
  void storeBeforeRead(int nth, std::optional<StoredState> state);
  void releaseAll();

 private:
  std::mutex mutex_;
  std::optional<StoredState> stored_;
  std::atomic<int> writes_{0};
  std::atomic<bool> hangRead_{false};
  std::atomic<bool> holdWrite_{false};
  int storeAt_ = 0;
  std::optional<StoredState> storeBefore_;
  Gate readGate_;
  Gate writeGate_;
};

class MemoryRegistry final : public keygrant::RegistryStash {
 public:
  std::optional<std::int64_t> readTrialStart() override;
  void writeTrialStart(std::int64_t ms) override;
  std::optional<std::int64_t> get();
  void set(std::optional<std::int64_t> value);
  std::atomic<bool> failReads{false};
  std::atomic<bool> failWrites{false};
  /// How many times the stash was read.
  std::atomic<int> reads{0};

 private:
  std::mutex mutex_;
  std::optional<std::int64_t> value_;
};

class FakeClock final : public keygrant::Clock {
 public:
  std::int64_t now() override {
    if (failing.load()) throw std::runtime_error("the clock cannot be read");
    return value.load();
  }
  std::atomic<std::int64_t> value{NOW};
  /// Every read throws.
  std::atomic<bool> failing{false};
};

/** A fingerprinter with get() only (unless given an identity's kind and alternates). */
class FakeFingerprinter final : public keygrant::Fingerprinter {
 public:
  explicit FakeFingerprinter(std::string fingerprint = "fp-test");
  std::string get() override;
  std::optional<keygrant::DeviceIdentity> identify(Millis timeout) override;
  void set(std::string fingerprint);
  /// Answer identify() with this kind and these alternates.
  void identifyAs(DeviceKind kind, std::vector<std::string> alternates);
  std::atomic<bool> failing{false};
  std::atomic<bool> hanging{false};
  std::atomic<int> calls{0};
  Gate& gate() { return gate_; }

 private:
  std::mutex mutex_;
  std::string fingerprint_;
  std::optional<DeviceKind> kind_;
  std::vector<std::string> alternates_;
  Gate gate_;
};

/**
 * A fingerprinter whose machine id never reads (no own fingerprint, the host
 * name as an alternate), recording the timeout each identify() is given: what
 * pins the bound a call reads the device for.
 */
class TimedId final : public keygrant::Fingerprinter {
 public:
  std::string get() override { return "unused"; }
  std::optional<keygrant::DeviceIdentity> identify(Millis timeout) override;
  /// Every timeout identify() was given, in order.
  std::vector<Millis> timeouts();

 private:
  std::mutex mutex_;
  std::vector<Millis> timeouts_;
};

/** HTTP answered by a handler per action, recording every call. */
class FakeHttp final : public keygrant::HttpAdapter {
 public:
  using Handler = std::function<keygrant::HttpResponse(const json& body)>;

  keygrant::HttpResponse post(const std::string& url, const std::string& body, Millis timeout) override;

  void on(const std::string& action, Handler handler);
  /// Answer `action` with this status and JSON body.
  void reply(const std::string& action, int status, const json& body);
  bool called(const std::string& action);
  int count(const std::string& action);
  /// The body of the first `action` call (a test failure when there was none).
  json bodyOf(const std::string& action);
  /// The URL the first request for `action` went to ("" when there was none).
  std::string urlOf(const std::string& action);
  /// The body of every `action` call, in order.
  std::vector<json> bodiesOf(const std::string& action);
  std::vector<std::string> actions();
  std::vector<Millis> timeouts();
  /// Wait until `action` has been called `times` times.
  bool waitFor(const std::string& action, int times = 1, Millis timeout = Millis{5000});

  /// Every call throws: no network.
  std::atomic<bool> fail{false};
  /// Every call hangs until released: a network that takes the connection and says nothing.
  std::atomic<bool> hang{false};
  Gate& hangGate() { return hangGate_; }

 private:
  struct Call {
    std::string action;
    json body;
    Millis timeout;
    std::string url;
  };
  std::mutex mutex_;
  std::condition_variable changed_;
  std::map<std::string, Handler> handlers_;
  std::vector<Call> calls_;
  Gate hangGate_;
};

/** What a harness is built with. */
struct HarnessOptions {
  std::int64_t major = 1;
  std::optional<std::string> deviceName;
  std::optional<Millis> httpTimeout;
  std::shared_ptr<keygrant::Fingerprinter> fingerprint;
  std::optional<bool> deviceHold;
  keygrant::detail::Timings timings{};
  /// The storage the License is given, made over the harness's in-memory one (default: that one itself).
  std::function<std::shared_ptr<keygrant::StorageAdapter>(std::shared_ptr<MemoryStorage>)> wrapStorage;
};

/** Short timings, for the tests that wait out a bound. */
keygrant::detail::Timings shortTimings();

/** A License over fake adapters and a fresh signing key (the reference's harness). */
class Harness {
 public:
  explicit Harness(std::optional<StoredState> initial = std::nullopt, HarnessOptions options = {});
  ~Harness();
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  keygrant::License& license() { return *license_; }
  /// Another build of the app on this device: the same adapters, another major.
  std::unique_ptr<keygrant::License> licenseFor(std::int64_t major);
  /// Another License on the same adapters, with another fingerprinter.
  std::unique_ptr<keygrant::License> another(std::shared_ptr<keygrant::Fingerprinter> fingerprint);

  /// A lease as the reference's harness signs one: sub, model, ttl, signed at `now`.
  std::string sign(const std::string& sub, const std::string& model, std::int64_t ttlSeconds,
                   std::int64_t now = NOW) const;
  std::string sign(const LeaseSpec& spec) const;

  /**
   * Whether the launch report the License has in flight, if any, settled
   * within `timeout` (the engine's test hook, not public API).
   */
  bool reportSettled(Millis timeout = Millis{10'000});

  std::optional<StoredState> stored() { return storage->get(); }
  void store(std::optional<StoredState> state) { storage->set(std::move(state)); }
  void setClock(std::int64_t now) { clock->value = now; }

  std::shared_ptr<MemoryStorage> storage = std::make_shared<MemoryStorage>();
  std::shared_ptr<MemoryRegistry> registry = std::make_shared<MemoryRegistry>();
  std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
  std::shared_ptr<keygrant::Fingerprinter> fingerprint;
  std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
  TestKey key;
  keygrant::LicenseConfig config;
  keygrant::detail::Timings timings;

 private:
  std::unique_ptr<keygrant::License> license_;
};

/** A state holding a key, an activation and a lease (the reference's common setStored). */
StoredState held(const std::string& lease, const std::string& key = KEY, std::optional<std::int64_t> checkedMajor = 1);

/** Elapsed real time of `work`. */
Millis timed(const std::function<void()>& work);

/** JSON text of arrays nested `depth` deep, `[[[...]]]`: deeper than any stack copying it recursively survives. */
std::string nestedArrays(std::size_t depth);

/** The depths a body or a state is nested to in the tests that it is no crash: past a 1 MB stack's, and past an 8 MB one's. */
inline constexpr std::size_t kDeepNesting[] = {20'000, 400'000};

}  // namespace kgtest

namespace keygrant {
// How GoogleTest prints these in a failure.
inline void PrintTo(const MachineIdReading& r, std::ostream* out) {
  *out << (r.kind == MachineIdReading::Kind::Id ? "id " + r.id
                                                 : r.kind == MachineIdReading::Kind::Unavailable ? "unavailable" : "transient");
}
inline void PrintTo(const DeviceIdentity& d, std::ostream* out) {
  *out << "{ fingerprint: " << d.fingerprint.value_or("(none)") << ", kind: " << (d.kind ? to_string(*d.kind) : "(none)")
       << ", alternates: " << d.alternates.size() << " }";
}
inline void PrintTo(const StoredState& s, std::ostream* out) { *out << serializeState(s); }
}  // namespace keygrant
