#include "support.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "crypto.hpp"
#include "engine.hpp"
#include "state_json.hpp"

namespace kgtest {

// --- statuses ----------------------------------------------------------------------

LicenseStatus status(LicenseState state) {
  LicenseStatus s;
  s.state = state;
  return s;
}

LicenseStatus status(LicenseState state, LicenseReason reason) {
  LicenseStatus s = status(state);
  s.reason = reason;
  return s;
}

LicenseStatus status(LicenseState state, LicenseReason reason, const std::string& key) {
  LicenseStatus s = status(state, reason);
  s.key = key;
  return s;
}

LicenseStatus licensed(const std::string& key, bool test) {
  LicenseStatus s = status(LicenseState::Licensed);
  s.key = key;
  s.test = test;
  return s;
}

LicenseStatus stalled(LicenseStatus s) {
  s.stalled = true;
  return s;
}

ActivateResult ok() { return ActivateResult::success(); }
ActivateResult failed(ActivateError error) { return ActivateResult::failure(error); }

// --- signing -------------------------------------------------------------------------

namespace {

struct PkeyDeleter {
  void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

std::shared_ptr<EVP_PKEY> own(EVP_PKEY* key) {
  if (key == nullptr) throw std::runtime_error("OpenSSL could not make an Ed25519 key");
  return std::shared_ptr<EVP_PKEY>(key, PkeyDeleter{});
}

}  // namespace

TestKey::TestKey() {
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  EVP_PKEY* key = nullptr;
  if (ctx == nullptr || EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &key) != 1) key = nullptr;
  EVP_PKEY_CTX_free(ctx);
  key_ = own(key);
}

TestKey TestKey::fromPrivate(const std::string& d) {
  const auto raw = keygrant::detail::base64UrlDecode(d);
  if (!raw || raw->size() != 32) throw std::runtime_error("not a private key");
  TestKey out;
  out.key_ = own(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, raw->data(), raw->size()));
  return out;
}

std::string TestKey::publicJwk() const {
  unsigned char raw[32];
  std::size_t size = sizeof raw;
  if (EVP_PKEY_get_raw_public_key(key_.get(), raw, &size) != 1) throw std::runtime_error("no public key");
  ordered_json jwk = {{"kty", "OKP"}, {"crv", "Ed25519"}, {"x", keygrant::detail::base64UrlEncode(raw, size)}};
  return jwk.dump();
}

std::string TestKey::signRaw(const std::string& payload) const {
  const std::string header = keygrant::detail::base64UrlEncode(std::string(R"({"alg":"EdDSA","typ":"JWT"})"));
  const std::string input = header + "." + keygrant::detail::base64UrlEncode(payload);
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  unsigned char signature[64];
  std::size_t size = sizeof signature;
  const bool signedOk = ctx != nullptr && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key_.get()) == 1 &&
                        EVP_DigestSign(ctx, signature, &size, reinterpret_cast<const unsigned char*>(input.data()),
                                       input.size()) == 1;
  EVP_MD_CTX_free(ctx);
  if (!signedOk) throw std::runtime_error("signing failed");
  return input + "." + keygrant::detail::base64UrlEncode(signature, size);
}

std::string TestKey::sign(const ordered_json& payload) const { return signRaw(payload.dump()); }

ordered_json claimsOf(const LeaseSpec& spec) {
  const std::int64_t iat = spec.now / 1000;
  ordered_json claims = {
      {"sub", spec.sub}, {"product", spec.product}, {"aid", spec.aid}, {"model", spec.model},
      {"lim", spec.lim}, {"major", spec.major},     {"ent", json::object()},
  };
  for (const auto& [name, value] : spec.extra.items()) claims[name] = value;
  claims["iss"] = "keygrant";
  claims["iat"] = iat;
  claims["exp"] = iat + spec.ttlSeconds;
  return claims;
}

std::string claimOf(const std::string& fingerprint) { return keygrant::detail::fingerprintClaim(fingerprint); }

// --- vectors ------------------------------------------------------------------------------

const json& vectors() {
  static const json loaded = [] {
    const char* override = std::getenv("KEYGRANT_VECTORS");
    const std::string path = override != nullptr ? override : KEYGRANT_VECTORS_PATH;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read the conformance vectors at " + path);
    std::stringstream text;
    text << in.rdbuf();
    return json::parse(text.str());
  }();
  return loaded;
}

StoredState stateFrom(const json& object) { return keygrant::detail::stateFromJson(object); }

// --- fakes ---------------------------------------------------------------------------------

void Gate::open() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
  }
  changed_.notify_all();
}

void Gate::wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  reached_ = true;
  changed_.notify_all();
  changed_.wait(lock, [&] { return open_; });
}

bool Gate::reached() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return reached_;
}

bool Gate::waitReached(Millis timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return changed_.wait_for(lock, timeout, [&] { return reached_; });
}

std::optional<StoredState> MemoryStorage::read(bool) {
  if (hangRead_.exchange(false)) readGate_.wait();
  if (failReads || failNextRead.exchange(false)) throw keygrant::IoError("EBUSY", "the disk said no");
  std::lock_guard<std::mutex> lock(mutex_);
  if (storeAt_ > 0 && --storeAt_ == 0) stored_ = std::move(storeBefore_);
  return stored_;
}

void MemoryStorage::storeBeforeRead(int nth, std::optional<StoredState> state) {
  std::lock_guard<std::mutex> lock(mutex_);
  storeAt_ = nth;
  storeBefore_ = std::move(state);
}

void MemoryStorage::write(const StoredState& state) {
  ++writes_;
  if (failWrites) throw keygrant::IoError("EBUSY", "the disk said no");
  if (holdWrite_.exchange(false)) writeGate_.wait();
  std::lock_guard<std::mutex> lock(mutex_);
  stored_ = state;
}

std::optional<StoredState> MemoryStorage::get() {
  std::lock_guard<std::mutex> lock(mutex_);
  return stored_;
}

void MemoryStorage::set(std::optional<StoredState> state) {
  std::lock_guard<std::mutex> lock(mutex_);
  stored_ = std::move(state);
}

void MemoryStorage::hangNextRead() { hangRead_ = true; }
void MemoryStorage::holdNextWrite() { holdWrite_ = true; }

void MemoryStorage::releaseAll() {
  readGate_.open();
  writeGate_.open();
}

std::optional<std::int64_t> MemoryRegistry::readTrialStart() {
  ++reads;
  if (failReads) throw std::runtime_error("the registry said no");
  std::lock_guard<std::mutex> lock(mutex_);
  return value_;
}

void MemoryRegistry::writeTrialStart(std::int64_t ms) {
  if (failWrites) throw std::runtime_error("the registry said no");
  std::lock_guard<std::mutex> lock(mutex_);
  value_ = ms;
}

std::optional<std::int64_t> MemoryRegistry::get() {
  std::lock_guard<std::mutex> lock(mutex_);
  return value_;
}

void MemoryRegistry::set(std::optional<std::int64_t> value) {
  std::lock_guard<std::mutex> lock(mutex_);
  value_ = value;
}

FakeFingerprinter::FakeFingerprinter(std::string fingerprint) : fingerprint_(std::move(fingerprint)) {}

std::string FakeFingerprinter::get() {
  ++calls;
  if (hanging) {
    gate_.wait();
    throw std::runtime_error("abandoned");
  }
  if (failing) throw std::runtime_error("fingerprint adapter failed");
  std::lock_guard<std::mutex> lock(mutex_);
  return fingerprint_;
}

std::optional<keygrant::DeviceIdentity> FakeFingerprinter::identify(Millis) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!kind_) return std::nullopt;
  ++calls;
  return keygrant::DeviceIdentity{fingerprint_, kind_, alternates_};
}

void FakeFingerprinter::set(std::string fingerprint) {
  std::lock_guard<std::mutex> lock(mutex_);
  fingerprint_ = std::move(fingerprint);
}

void FakeFingerprinter::identifyAs(DeviceKind kind, std::vector<std::string> alternates) {
  std::lock_guard<std::mutex> lock(mutex_);
  kind_ = kind;
  alternates_ = std::move(alternates);
}

std::optional<keygrant::DeviceIdentity> TimedId::identify(Millis timeout) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    timeouts_.push_back(timeout);
  }
  keygrant::DeviceIdentity identity;
  identity.alternates = {"fp-host"};
  return identity;
}

std::vector<Millis> TimedId::timeouts() {
  std::lock_guard<std::mutex> lock(mutex_);
  return timeouts_;
}

keygrant::HttpResponse FakeHttp::post(const std::string& url, const std::string& body, Millis timeout) {
  const std::string action = url.substr(url.rfind('/') + 1);
  Handler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    calls_.push_back(Call{action, json::parse(body), timeout});
    if (const auto it = handlers_.find(action); it != handlers_.end()) handler = it->second;
  }
  changed_.notify_all();
  if (fail) throw std::runtime_error("network down");
  if (hang) {
    hangGate_.wait();
    throw std::runtime_error("abandoned");
  }
  if (!handler) return keygrant::HttpResponse{404, R"({"error":"unknown"})"};
  return handler(json::parse(body));
}

void FakeHttp::on(const std::string& action, Handler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_[action] = std::move(handler);
}

void FakeHttp::reply(const std::string& action, int status, const json& body) {
  const std::string text = body.dump();
  on(action, [status, text](const json&) { return keygrant::HttpResponse{status, text}; });
}

bool FakeHttp::called(const std::string& action) { return count(action) > 0; }

int FakeHttp::count(const std::string& action) {
  std::lock_guard<std::mutex> lock(mutex_);
  int n = 0;
  for (const Call& call : calls_) n += call.action == action ? 1 : 0;
  return n;
}

json FakeHttp::bodyOf(const std::string& action) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Call& call : calls_) {
    if (call.action == action) return call.body;
  }
  ADD_FAILURE() << "expected a " << action << " call";
  return json();
}

std::vector<json> FakeHttp::bodiesOf(const std::string& action) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<json> out;
  for (const Call& call : calls_) {
    if (call.action == action) out.push_back(call.body);
  }
  return out;
}

std::vector<std::string> FakeHttp::actions() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  for (const Call& call : calls_) out.push_back(call.action);
  return out;
}

std::vector<Millis> FakeHttp::timeouts() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Millis> out;
  for (const Call& call : calls_) out.push_back(call.timeout);
  return out;
}

bool FakeHttp::waitFor(const std::string& action, int times, Millis timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return changed_.wait_for(lock, timeout, [&] {
    int n = 0;
    for (const Call& call : calls_) n += call.action == action ? 1 : 0;
    return n >= times;
  });
}

// --- the harness ---------------------------------------------------------------------------

keygrant::detail::Timings shortTimings() {
  keygrant::detail::Timings t;
  t.lockGrace = Millis{150};
  t.statusCheck = Millis{150};
  t.machineId = Millis{100};
  t.activateMachineId = Millis{400};
  // Longer than the machine-id read's own slack (500 ms): an identify() bounded
  // by its reader answers its host-name alternate before this bound abandons it.
  t.identifyExtra = Millis{1000};
  return t;
}

Harness::Harness(std::optional<StoredState> initial, HarnessOptions options)
    : fingerprint(options.fingerprint ? options.fingerprint : std::make_shared<FakeFingerprinter>()),
      timings(options.timings) {
  storage->set(std::move(initial));
  config.product = PRODUCT;
  config.apiBaseUrl = "https://api.test/";
  config.publicJwk = key.publicJwk();
  config.major = options.major;
  config.deviceName = options.deviceName;
  if (options.httpTimeout) config.httpTimeout = *options.httpTimeout;
  if (options.deviceHold) config.deviceHold = *options.deviceHold;
  config.adapters = keygrant::Adapters{storage, registry, clock, fingerprint, http};
  license_ = std::make_unique<keygrant::License>(config, timings);
}

Harness::~Harness() {
  // Lets go of anything a fake still holds, so no thread is left waiting on it.
  storage->releaseAll();
  http->hangGate().open();
  if (auto* fake = dynamic_cast<FakeFingerprinter*>(fingerprint.get())) fake->gate().open();
}

bool Harness::reportSettled(Millis timeout) {
  return keygrant::detail::LicenseAccess::launchReportSettled(*license_, timeout);
}

std::unique_ptr<keygrant::License> Harness::licenseFor(std::int64_t major) {
  keygrant::LicenseConfig other = config;
  other.major = major;
  other.deviceName.reset();
  return std::make_unique<keygrant::License>(other, timings);
}

std::unique_ptr<keygrant::License> Harness::another(std::shared_ptr<keygrant::Fingerprinter> fingerprinter) {
  keygrant::LicenseConfig other = config;
  other.adapters.fingerprint = std::move(fingerprinter);
  return std::make_unique<keygrant::License>(other, timings);
}

std::string Harness::sign(const std::string& sub, const std::string& model, std::int64_t ttlSeconds,
                          std::int64_t now) const {
  LeaseSpec spec;
  spec.sub = sub;
  spec.model = model;
  spec.ttlSeconds = ttlSeconds;
  spec.now = now;
  return sign(spec);
}

std::string Harness::sign(const LeaseSpec& spec) const { return key.sign(claimsOf(spec)); }

StoredState held(const std::string& lease, const std::string& key, std::optional<std::int64_t> checkedMajor) {
  StoredState s;
  s.key = key;
  s.activationId = "act_1";
  s.lease = lease;
  s.checkedMajor = checkedMajor;
  s.lastSeen = NOW;
  return s;
}

Millis timed(const std::function<void()>& work) {
  const auto start = std::chrono::steady_clock::now();
  work();
  return std::chrono::duration_cast<Millis>(std::chrono::steady_clock::now() - start);
}

std::string nestedArrays(std::size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); }

}  // namespace kgtest
