// The journal over the default file storage: a state file whose journal is not one, and two instances of the app over one
// file, which never lose each other's spends and judge the allowance where a spend lands.
#include <algorithm>
#include <future>

#include "memory_fs.hpp"
#include "meter_support.hpp"

namespace kgtest {
namespace {

using Op = MemoryFs::Op;
using namespace std::chrono_literals;

/** One state file in memory, and instances of the app over it: the harness's key, clock, server and device. */
class SharedFile {
 public:
  explicit SharedFile(const MeterSpecs& meters, const std::function<void(StoredState&)>& adjust = {})
      : memory(std::make_shared<MemoryFs>()) {
    StoredState state = meterState(h, meters);
    state.rev = 1;
    if (adjust) adjust(state);
    memory->put(memoryState(), keygrant::serializeState(state));
  }

  std::unique_ptr<keygrant::License> instance() {
    keygrant::LicenseConfig config = h.config;
    config.adapters.storage = storageOver(memory);
    return std::make_unique<keygrant::License>(config, h.timings);
  }

  StoredState saved() { return *keygrant::parseState(*memory->file(memoryState())); }

  Harness h;
  std::shared_ptr<MemoryFs> memory;
};

/// `first` runs; while it is making its first temp file, `between` runs (another instance's whole call), then `first` goes on.
template <class T>
T interleave(MemoryFs& memory, std::function<T()> first, const std::function<void()>& between) {
  const auto saving = memory.holdNext(Op::Create);
  auto running = std::async(std::launch::async, std::move(first));
  EXPECT_TRUE(saving->waitReached(5s));
  between();
  saving->open();
  return running.get();
}

TEST(UseProcess, ADeactivateReleasesAllTheSameOverAFileWhoseJournalIsNotOneAndWritesNoneOfItBack) {
  Harness h;
  const std::string lease = meterLease(h);
  const json junk = {{"key", KEY},
                     {"activationId", "act_1"},
                     {"lease", lease},
                     {"lastSeen", NOW},
                     {"rev", 1},
                     {"spends", json::array({{{"id", 4}}, nullptr, "x", {{"id", "a"}, {"meter", "exports"}, {"n", 0}, {"at", 1}, {"kind", "offline"}}})},
                     {"meterFigures", json::array({1, 2})}};
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), junk.dump()}});
  keygrant::LicenseConfig config = h.config;
  config.adapters.storage = storageOver(memory);
  keygrant::License license(config, h.timings);
  h.http->reply("deactivate", 200, json::object());
  // It reads, shows no meters' journal, renews all the same, and releases with only the key and the seat.
  EXPECT_EQ(license.status().state, LicenseState::Licensed);
  EXPECT_EQ(license.deactivate(), keygrant::DeactivateResult{true});
  EXPECT_EQ(h.http->bodyOf("deactivate"), (json{{"key", KEY}, {"activationId", "act_1"}}));
  const json saved = json::parse(*memory->file(memoryState()));
  EXPECT_FALSE(saved.contains("spends"));
  EXPECT_FALSE(saved.contains("meterFigures"));
}

TEST(UseProcess, AJunkJournalIsNotWrittenBackByASpendEither) {
  Harness h;
  const json junk = {{"key", KEY}, {"activationId", "act_1"}, {"lease", meterLease(h)}, {"lastSeen", NOW}, {"rev", 1},
                     {"spends", "junk"}, {"meterFigures", 5}};
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), junk.dump()}});
  keygrant::LicenseConfig config = h.config;
  config.adapters.storage = storageOver(memory);
  keygrant::License license(config, h.timings);
  h.http->fail = true;
  EXPECT_EQ(license.use("exports"), UseResult::spent(89, true));
  const json saved = json::parse(*memory->file(memoryState()));
  ASSERT_TRUE(saved["spends"].is_array());
  EXPECT_EQ(saved["spends"].size(), 1u);
  EXPECT_EQ(saved["spends"][0]["kind"], "unanswered");
  EXPECT_FALSE(saved.contains("meterFigures"));
}

TEST(UseProcess, TwoInstancesNeverLoseEachOthersSpendsASaveThatFindsTheFileChangedMergesOntoIt) {
  SharedFile file(exportsOnly(1'000, 1'000, 1'000));
  file.h.http->fail = true;
  const auto a = file.instance();
  const auto b = file.instance();
  // A's first save has read the file and is making its temp file: there, the other instance spends and saves, so A's
  // revision check finds the file changed and merges onto it.
  const UseResult first = interleave<UseResult>(*file.memory, [&] { return a->use("exports", 5); }, [&] {
    const UseResult theirs = b->use("exports", 7);
    EXPECT_TRUE(theirs.ok);
    EXPECT_TRUE(theirs.offline);
  });
  EXPECT_TRUE(first.ok);
  EXPECT_TRUE(first.offline);
  const StoredState saved = file.saved();
  std::vector<std::pair<std::string, std::int64_t>> spends;
  for (const SpendEntry& e : saved.spends) spends.emplace_back(keygrant::to_string(e.kind), e.n);
  std::sort(spends.begin(), spends.end());
  EXPECT_EQ(spends, (std::vector<std::pair<std::string, std::int64_t>>{{"unanswered", 5}, {"unanswered", 7}}));
  EXPECT_EQ(a->status().meters.at("exports").unsynced, 12);
  EXPECT_EQ(b->status().meters.at("exports").unsynced, 12);
}

TEST(UseProcess, ASpendTheOtherInstanceMadeIsCountedAgainstTheAllowanceWhereTheSpendLands) {
  SharedFile file(exportsOnly(90, 100, 10));
  file.h.http->fail = true;
  const auto a = file.instance();
  const auto b = file.instance();
  // A read the journal empty, so the allowance covers its 5; by the time it lands B has spent 8: 13 would pass 10.
  const UseResult refused = interleave<UseResult>(*file.memory, [&] { return a->use("exports", 5); }, [&] {
    EXPECT_EQ(b->use("exports", 8), UseResult::spent(82, true));
  });
  // What is left is as the landing found it: the 90, less the 8 B had spent by then.
  EXPECT_EQ(refused, UseResult::refused(UseError::OfflineLimit, 82));
  const StoredState saved = file.saved();
  std::int64_t unsynced = 0;
  int voids = 0;
  for (const SpendEntry& e : saved.spends) {
    if (e.kind == SpendKind::Void) {
      ++voids;
    } else {
      unsynced += e.n;
    }
  }
  EXPECT_EQ(unsynced, 8);
  EXPECT_EQ(voids, 1);
  EXPECT_EQ(a->status().meters.at("exports").unsynced, 8);
}

TEST(UseProcess, ACallBlocksAndRunsToItsBoundedAnswerWhateverTheCallerDoes) {
  SharedFile file(exportsOnly(90, 100, 10));
  file.h.http->hang = true;
  keygrant::LicenseConfig config = file.h.config;
  config.adapters.storage = storageOver(file.memory);
  config.httpTimeout = Millis{150};
  keygrant::License license(config, shortTimings());
  UseResult result;
  const Millis took = timed([&] { result = license.use("exports"); });
  EXPECT_EQ(result, UseResult::spent(89, true));
  EXPECT_GE(took, Millis{140});
  EXPECT_LT(took, Millis{4'000});
  // The spend in doubt is stored: the bounded answer cost the customer nothing.
  EXPECT_EQ(file.saved().spends.size(), 1u);
  EXPECT_EQ(file.saved().spends[0].kind, SpendKind::Unanswered);
}

}  // namespace
}  // namespace kgtest
