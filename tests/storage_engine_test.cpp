// A License over the default storage: storage that cannot be read, calls behind a stuck read, and
// saves from more than one process or instance of the app on the same file.
#include <future>

#include "memory_fs.hpp"
#include "offline.hpp"

namespace kgtest {
namespace {

using keygrant::FileStorage;
using keygrant::IoError;
using Op = MemoryFs::Op;
using namespace std::chrono_literals;

/** HTTP that answers each action as given (404 otherwise), recording every URL. */
class ScriptedHttp final : public keygrant::HttpAdapter {
 public:
  explicit ScriptedHttp(std::map<std::string, keygrant::HttpResponse> answers) : answers_(std::move(answers)) {}
  keygrant::HttpResponse post(const std::string& url, const std::string&, Millis) override {
    std::lock_guard<std::mutex> lock(mutex_);
    asked.push_back(url);
    const auto it = answers_.find(url.substr(url.rfind('/') + 1));
    return it != answers_.end() ? it->second : keygrant::HttpResponse{404, "{}"};
  }
  std::vector<std::string> asked;

 private:
  std::mutex mutex_;
  std::map<std::string, keygrant::HttpResponse> answers_;
};

keygrant::HttpResponse answer(int status, const json& body) { return {status, body.dump()}; }

/** A License over the default storage on `memory`, its server answering 503, its leases signed by `key`. */
std::unique_ptr<keygrant::License> licenseOver(const std::shared_ptr<MemoryFs>& memory, std::shared_ptr<ScriptedHttp> http,
                                               std::optional<std::string> migrateFrom = std::nullopt,
                                               const TestKey& key = TestKey()) {
  keygrant::LicenseConfig config;
  config.product = PRODUCT;
  config.apiBaseUrl = "https://api.test";
  config.publicJwk = key.publicJwk();
  config.httpTimeout = Millis{100};
  config.adapters.storage = storageOver(memory, {}, std::move(migrateFrom));
  config.adapters.http = std::move(http);
  config.adapters.fingerprint = std::make_shared<FakeFingerprinter>();
  return std::make_unique<keygrant::License>(config, shortTimings());
}

std::shared_ptr<ScriptedHttp> unavailable() {
  return std::make_shared<ScriptedHttp>(std::map<std::string, keygrant::HttpResponse>{});
}

TEST(StorageUnreadable, StatusIsUnknownActivateAndDeactivateThrowNothingAskedOrWritten) {
  // Every save is read, patch, write: a read taken as "no state" would be
  // written back over the licence. And an app awaits status() at launch.
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), kSaved}});
  memory->failAlways(Op::ReadFile, "EBUSY");
  const auto http = unavailable();
  const auto license = licenseOver(memory, http);
  const LicenseStatus unknown = status(LicenseState::Unknown, LicenseReason::Storage);
  EXPECT_EQ(license->status(), unknown);
  EXPECT_EQ(license->refresh(), unknown);
  EXPECT_EQ(license->startTrial(), unknown);
  try {
    license->activate("SLUICE-EEEE-FFFF-GGGG-HHHH");
    FAIL() << "expected activate() to throw";
  } catch (const IoError& error) {
    EXPECT_EQ(error.code(), "EBUSY");
  }
  EXPECT_THROW(license->deactivate(), IoError);
  EXPECT_TRUE(http->asked.empty());
  EXPECT_EQ(memory->files(), (std::map<std::string, std::string>{{memoryState(), kSaved}}));
  EXPECT_TRUE(memory->changes().empty());
}

/** A status() stuck on its first read, and the next status() past its grace, read-only. */
struct BehindAStuckRead {
  std::shared_ptr<MemoryFs> memory;
  std::unique_ptr<keygrant::License> license;
  std::shared_ptr<Gate> stuck;
  std::future<LicenseStatus> first;
  LicenseStatus answer;

  explicit BehindAStuckRead(std::shared_ptr<MemoryFs> fs, std::optional<std::string> migrateFrom = std::nullopt,
                            const TestKey& key = TestKey())
      : memory(std::move(fs)), license(licenseOver(memory, unavailable(), std::move(migrateFrom), key)) {
    stuck = memory->holdNext(Op::ReadFile);
    first = std::async(std::launch::async, [this] { return license->status(); });
    EXPECT_TRUE(stuck->waitReached(5s));
    answer = license->status();
  }
  ~BehindAStuckRead() {
    stuck->open();
    first.wait();
  }
};

TEST(StorageBehindAStuckCall, DoesNotSetAFileThatDoesNotParseAsideNorWriteInItsPlace) {
  BehindAStuckRead behind(std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), "{\"bad"}}));
  EXPECT_EQ(behind.answer, stalled(status(LicenseState::Unlicensed)));
  EXPECT_TRUE(behind.memory->changes().empty());
}

TEST(StorageBehindAStuckCall, ReadsAnOlderSdksStateWithoutCopyingIt) {
  const std::string legacy = keygrant::detail::joinPath(memoryDir() + "-legacy", "sluice.json");
  const std::int64_t now = keygrant::SystemClock().now();
  // A trial, its lease signed to its end as the server signs it.
  const TestKey key;
  LeaseSpec spec;
  spec.sub = "trial:fp-test";
  spec.model = "trial";
  spec.now = now - DAY;
  spec.ttlSeconds = 14 * 24 * 3600;
  const json trial = {{"trialStart", now - DAY}, {"trialEndsAt", now + 13 * DAY}, {"trialLease", key.sign(claimsOf(spec))}};
  BehindAStuckRead behind(std::make_shared<MemoryFs>(std::map<std::string, std::string>{{legacy, trial.dump()}}), legacy,
                          key);
  EXPECT_EQ(behind.answer.state, LicenseState::Trial);
  EXPECT_TRUE(behind.answer.stalled);
  EXPECT_TRUE(behind.memory->changes().empty());
}

TEST(StorageBehindAStuckCall, IsUnknownStillWritingNothingWhenTheStateCannotBeRead) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), kSaved}});
  // The stuck read is held first, then fails like every read after it.
  memory->failAlways(Op::ReadFile, "EBUSY");
  BehindAStuckRead behind(memory);
  EXPECT_EQ(behind.answer, stalled(status(LicenseState::Unknown, LicenseReason::Storage)));
  EXPECT_TRUE(behind.memory->changes().empty());
}

TEST(StorageBehindAStuckCall, TheUpgradeLinkIsMadeFromAnOlderSdksStateWithoutCopyingItBesideAnyCall) {
  const std::string legacy = keygrant::detail::joinPath(memoryDir() + "-legacy", "sluice.json");
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{legacy, kSaved}});
  const auto license = licenseOver(memory, unavailable(), legacy);
  EXPECT_EQ(license->upgradeUrl("https://buy.test/upgrade"), "https://buy.test/upgrade?client_reference_id=act_1");
  EXPECT_TRUE(memory->changes().empty());
}

/** The engine's own save, as one process makes it through the storage's update(). */
void save(FileStorage& storage, const keygrant::detail::StatePatch& patch, const keygrant::detail::Basis& basis = {}) {
  storage.update([&](const StoredState& latest) {
    StoredState next = keygrant::detail::merged(latest, patch, basis);
    next.lastSeen = std::max(latest.lastSeen.value_or(0), keygrant::SystemClock().now());
    return next;
  });
}

/** `first` held at its first create until `between` has run, then let finish. */
template <class T>
T interleave(MemoryFs& memory, std::function<T()> first, const std::function<void()>& between) {
  const auto saving = memory.holdNext(Op::Create);
  auto running = std::async(std::launch::async, std::move(first));
  EXPECT_TRUE(saving->waitReached(5s));
  between();
  saving->open();
  return running.get();
}

json savedJson(MemoryFs& memory) { return json::parse(*memory.file(memoryState())); }

using keygrant::detail::Change;
using keygrant::detail::StatePatch;

TEST(SavesFromMoreThanOneProcess, LoseNeitherOfTwoSavesThatInterleave) {
  const std::vector<std::map<std::string, std::string>> starts = {
      {{memoryState(), R"({"key":"K","rev":4})"}},  // a stamped state
      {{memoryState(), R"({"key":"K"})"}},          // an older SDK's, never stamped
      {},                                           // no state yet
  };
  for (const auto& files : starts) {
    auto memory = std::make_shared<MemoryFs>(files);
    const auto a = storageOver(memory);
    const auto b = storageOver(memory);
    StatePatch kept;
    kept.keptMajor = Change<std::int64_t>::set(2);
    StatePatch checked;
    checked.checkedMajor = Change<std::int64_t>::set(3);
    interleave<int>(*memory, [&] { save(*a, kept); return 0; }, [&] { save(*b, checked); });
    const json saved = savedJson(*memory);
    EXPECT_EQ(saved["keptMajor"], 2);
    EXPECT_EQ(saved["checkedMajor"], 3);
    EXPECT_EQ(memory->files().size(), 1u);
  }
}

TEST(SavesFromMoreThanOneProcess, KeepWhatAnotherProcessRemovedInBetween) {
  using keygrant::detail::Asked;
  using keygrant::detail::Basis;
  struct Case {
    const char* name;
    StatePatch b;
    std::optional<Asked> bAsked;
    StatePatch a;
    std::optional<Asked> aAsked;
    std::vector<const char*> gone;
  };
  StatePatch clearRefusal;
  clearRefusal.refused = Change<keygrant::Refusal>::clear();
  StatePatch dropKey;
  dropKey.key = Change<std::string>::clear();
  dropKey.activationId = Change<std::string>::clear();
  StatePatch keptOnly;
  keptOnly.keptMajor = Change<std::int64_t>::set(2);
  StatePatch newer;
  newer.lease = Change<std::string>::set("L2");
  newer.refused = Change<keygrant::Refusal>::clear();
  StatePatch older = keptOnly;
  older.refused = Change<keygrant::Refusal>::set(keygrant::Refusal::Revoked);
  older.lease = Change<std::string>::clear();
  const std::vector<Case> cases = {
      {"B clears the refusal", clearRefusal, std::nullopt, keptOnly, std::nullopt, {"refused"}},
      {"B drops the key", dropKey, std::nullopt, keptOnly, std::nullopt, {"key", "activationId"}},
      {"B's answer, asked for later, clears the refusal A's older answer would set again", newer,
       Asked{2'000, "act_1", 100}, older, Asked{1'000, "act_1", 100}, {"refused"}},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{
        {memoryState(), R"({"key":"K","activationId":"act_1","refused":"revoked","verdictAt":100,"rev":4})"}});
    const auto a = storageOver(memory);
    const auto b = storageOver(memory);
    Basis aBasis;
    aBasis.answer = c.aAsked;
    Basis bBasis;
    bBasis.answer = c.bAsked;
    interleave<int>(*memory, [&] { save(*a, c.a, aBasis); return 0; }, [&] { save(*b, c.b, bBasis); });
    const json saved = savedJson(*memory);
    EXPECT_EQ(saved["keptMajor"], 2);
    for (const char* field : c.gone) EXPECT_FALSE(saved.contains(field)) << field;
    if (c.bAsked) {
      EXPECT_EQ(saved["lease"], "L2");
      EXPECT_EQ(saved["verdictAt"], 2'000);
    }
  }
}

TEST(SavesFromMoreThanOneProcess, KeepTheHigherKeptMajorAnotherProcessNotedInBetween) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{
      {memoryState(), R"({"key":"K","activationId":"act_1","keptMajor":2,"rev":1})"}});
  const auto a = storageOver(memory);
  const auto b = storageOver(memory);
  keygrant::detail::Basis about;
  about.about = keygrant::detail::About{"act_1"};
  StatePatch three;
  three.keptMajor = Change<std::int64_t>::set(3);
  StatePatch five;
  five.keptMajor = Change<std::int64_t>::set(5);
  interleave<int>(*memory, [&] { save(*a, three, about); return 0; }, [&] { save(*b, five, about); });
  EXPECT_EQ(savedJson(*memory)["keptMajor"], 5);
}

/** One state file, a signing key, and instances of the app over them. */
class OneFile {
 public:
  explicit OneFile(const std::function<StoredState(OneFile&)>& stored) {
    memory = std::make_shared<MemoryFs>();
    memory->put(memoryState(), keygrant::serializeState(stored(*this)));
  }

  std::string sign(std::int64_t at, ordered_json extra = ordered_json::object(), std::string sub = KEY,
                   std::string aid = "act_1") const {
    LeaseSpec spec;
    spec.sub = std::move(sub);
    spec.aid = std::move(aid);
    spec.now = at;
    spec.extra = std::move(extra);
    return key.sign(claimsOf(spec));
  }

  /** An instance whose clock reads `now` and whose server answers as given. */
  std::unique_ptr<keygrant::License> instance(std::int64_t now, std::map<std::string, keygrant::HttpResponse> answers,
                                              std::shared_ptr<keygrant::Fingerprinter> fingerprint = nullptr) {
    keygrant::LicenseConfig config;
    config.product = PRODUCT;
    config.apiBaseUrl = "https://api.test";
    config.publicJwk = key.publicJwk();
    config.adapters.storage = storageOver(memory);
    config.adapters.http = std::make_shared<ScriptedHttp>(std::move(answers));
    auto clock = std::make_shared<FakeClock>();
    clock->value = now;
    config.adapters.clock = clock;
    config.adapters.fingerprint = fingerprint ? std::move(fingerprint) : std::make_shared<FakeFingerprinter>();
    return std::make_unique<keygrant::License>(config);
  }

  StoredState saved() { return *keygrant::parseState(*memory->file(memoryState())); }

  TestKey key;
  std::shared_ptr<MemoryFs> memory;
};

StoredState heldSince(OneFile& file, ordered_json extra = ordered_json::object()) {
  StoredState s = held(file.sign(NOW - DAY, std::move(extra)));
  s.lastSeen = NOW - DAY;
  s.rev = 1;
  return s;
}

TEST(AnswersSavedByTwoInstances, KeepTheNewerAnswerARefusalAskedLaterSurvivesAnOlderAsksLease) {
  OneFile file([](OneFile& f) { return heldSince(f); });
  // A asks first and is answered with a lease; B asks a second later and is refused.
  const auto a = file.instance(NOW, {{"validate", answer(200, {{"lease", file.sign(NOW)}, {"activationId", "act_1"}})}});
  const auto b = file.instance(NOW + 1'000, {{"validate", answer(403, {{"error", "revoked"}})}});
  const LicenseStatus answered = interleave<LicenseStatus>(*file.memory, [&] { return a->refresh(); }, [&] {
    EXPECT_EQ(b->refresh(), status(LicenseState::Invalid, LicenseReason::Revoked, KEY));
  });
  EXPECT_EQ(answered, licensed(KEY));
  EXPECT_EQ(file.saved().refused, keygrant::Refusal::Revoked);
  EXPECT_EQ(file.saved().verdictAt, NOW + 1'000);
  EXPECT_EQ(file.saved().lease, std::nullopt);
  EXPECT_EQ(file.memory->count(Op::Create), 3);
}

TEST(AnswersSavedByTwoInstances, KeepTheLaterAsksAnswerWhenTheClockWasPutBackBetweenTheAsks) {
  OneFile file([](OneFile& f) { return heldSince(f); });
  const auto a = file.instance(NOW, {{"validate", answer(200, {{"lease", file.sign(NOW)}, {"activationId", "act_1"}})}});
  // B asks after the clock was put back a minute: its own clock reads earlier than A's ask.
  const auto b = file.instance(NOW - 60'000, {{"validate", answer(403, {{"error", "revoked"}})}});
  interleave<LicenseStatus>(*file.memory, [&] { return a->refresh(); }, [&] {
    // A save made at NOW + 2 s, before the clock went back, raised the guard.
    StoredState raised = file.saved();
    raised.lastSeen = NOW + 2'000;
    raised.rev = raised.rev.value_or(0) + 1;
    file.memory->put(memoryState(), keygrant::serializeState(raised));
    b->refresh();
  });
  // Stamped by the guarded clock, B's answer still reads as the later one.
  EXPECT_EQ(file.saved().refused, keygrant::Refusal::Revoked);
  EXPECT_EQ(file.saved().verdictAt, NOW + 2'000);
  EXPECT_EQ(file.saved().lease, std::nullopt);
}

TEST(AnswersSavedByTwoInstances, KeepARefusalAskedLaterOverAReactivationOfTheSameSeatThatSavesAfterIt) {
  OneFile file([](OneFile& f) { return heldSince(f); });
  const auto w1 = file.instance(NOW, {{"activate", answer(200, {{"lease", file.sign(NOW)}, {"activationId", "act_1"}})}});
  const auto w2 = file.instance(NOW + 1'000, {{"validate", answer(403, {{"error", "revoked"}})}});
  const ActivateResult activated = interleave<ActivateResult>(*file.memory, [&] { return w1->activate(KEY); }, [&] {
    EXPECT_EQ(w2->refresh(), status(LicenseState::Invalid, LicenseReason::Revoked, KEY));
  });
  EXPECT_EQ(activated, ok());
  const StoredState saved = file.saved();
  EXPECT_EQ(saved.key, KEY);
  EXPECT_EQ(saved.activationId, "act_1");
  EXPECT_EQ(saved.refused, keygrant::Refusal::Revoked);
  EXPECT_EQ(saved.verdictAt, NOW + 1'000);
  EXPECT_EQ(saved.lease, std::nullopt);
  // So the revoked device does not license offline.
  const LicenseStatus after = w1->status();
  EXPECT_EQ(after.state, LicenseState::Invalid);
  EXPECT_EQ(after.reason, LicenseReason::Revoked);
}

TEST(AnswersSavedByTwoInstances, ARenewalThatLandsAfterItsActivationIsGoneWritesNothingOfIt) {
  const ordered_json open = {{"major", keygrant::kOpenMajor}, {"upgradeUntil", (NOW + 10 * DAY) / 1000}};
  auto machine = std::make_shared<FakeFingerprinter>("fp-machine");
  machine->identifyAs(DeviceKind::Machine, {});
  for (const char* action : {"deactivate", "activate"}) {
    SCOPED_TRACE(action);
    OneFile file([&](OneFile& f) { return heldSince(f, open); });
    const std::string leaseB = file.sign(NOW, ordered_json::object(), "SLUICE-BBBB-BBBB-BBBB-BBBB", "act_2");
    const auto w1 = file.instance(NOW, {{"deactivate", answer(200, {{"ok", true}})},
                                        {"activate", answer(200, {{"lease", leaseB}, {"activationId", "act_2"}})}});
    const auto w2 = file.instance(NOW, {{"validate", answer(200, {{"lease", file.sign(NOW, open)}, {"activationId", "act_1"}})}},
                                  machine);
    interleave<LicenseStatus>(*file.memory, [&] { return w2->refresh(); }, [&] {
      if (std::string(action) == "deactivate") {
        w1->deactivate();
      } else {
        EXPECT_EQ(w1->activate("SLUICE-BBBB-BBBB-BBBB-BBBB"), ok());
      }
    });
    const StoredState saved = file.saved();
    EXPECT_EQ(saved.keptMajor, std::nullopt);
    EXPECT_EQ(saved.deviceKind, std::nullopt);
    if (std::string(action) == "deactivate") {
      EXPECT_EQ(saved.key, std::nullopt);
    } else {
      EXPECT_EQ(saved.key, "SLUICE-BBBB-BBBB-BBBB-BBBB");
      EXPECT_EQ(saved.activationId, "act_2");
      EXPECT_EQ(saved.lease, leaseB);
    }
  }
}

}  // namespace
}  // namespace kgtest
