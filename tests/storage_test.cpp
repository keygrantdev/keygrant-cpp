// The default storage (sdk/src/storage.test.ts): where it lives, and how it is
// written and read so that neither a crash nor a busy disk costs a licence.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#endif

#include "memory_fs.hpp"
#include "paths.hpp"

namespace kgtest {
namespace {

namespace fs = std::filesystem;
using keygrant::FileStorage;
using keygrant::FileStorageOptions;
using keygrant::IoError;
using Op = MemoryFs::Op;

/** A folder for one test, removed after it. */
class TempDir {
 public:
  TempDir() {
    std::random_device random;
    path_ = fs::temp_directory_path() / ("keygrant-" + std::to_string(random()) + std::to_string(random()));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  std::string file(const std::string& name) const { return (path_ / name).string(); }
  std::vector<std::string> names() const {
    std::vector<std::string> out;
    for (const auto& entry : fs::directory_iterator(path_)) out.push_back(entry.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
  }

 private:
  fs::path path_;
};

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

void spit(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

/** Sets a file's mtime to `ms` since the epoch (POSIX only). */
bool age(const std::string& path, std::int64_t ms) {
#ifdef _WIN32
  (void)path;
  (void)ms;
  return false;
#else
  struct timespec times[2];
  times[0].tv_sec = times[1].tv_sec = static_cast<time_t>(ms / 1000);
  times[0].tv_nsec = times[1].tv_nsec = static_cast<long>((ms % 1000) * 1'000'000);
  return utimensat(AT_FDCWD, path.c_str(), times, 0) == 0;
#endif
}

StoredState keyed(const char* key, std::optional<std::int64_t> lastSeen = std::nullopt) {
  StoredState s;
  s.key = key;
  s.lastSeen = lastSeen;
  return s;
}

TEST(DefaultStoragePath, KeepsStatePerUserOnThisMachineNeverInTheWorkingDirectory) {
  using keygrant::SystemPaths;
  const auto windows = [](std::map<std::string, std::string> env) { return SystemPaths{"win32", "C:\\Users\\jo", std::move(env)}; };
  const auto posix = [](std::map<std::string, std::string> env) { return SystemPaths{"linux", "/home/jo", std::move(env)}; };
  const std::string local = "C:\\Users\\jo\\AppData\\Local\\KeyGrant\\sluice.json";
  struct Case {
    const char* name;
    SystemPaths system;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {"Windows", windows({{"LOCALAPPDATA", "C:\\Users\\jo\\AppData\\Local"}}), local},
      {"Windows elsewhere", windows({{"LOCALAPPDATA", "D:\\Profiles\\jo\\Local"}}), "D:\\Profiles\\jo\\Local\\KeyGrant\\sluice.json"},
      // Roaming AppData follows the user to every machine of a domain: never used.
      {"Windows with only the roaming %APPDATA%", windows({{"APPDATA", "C:\\Users\\jo\\AppData\\Roaming"}}), local},
      {"Windows with no %LOCALAPPDATA%", windows({}), local},
      {"Windows with it empty", windows({{"LOCALAPPDATA", ""}}), local},
      {"Windows with it relative", windows({{"LOCALAPPDATA", "AppData\\Local"}}), local},
      {"Windows with it drive-relative", windows({{"LOCALAPPDATA", "C:Local"}}), local},
      {"Windows with a trailing separator", windows({{"LOCALAPPDATA", "C:\\Users\\jo\\AppData\\Local\\"}}), local},
      {"macOS", SystemPaths{"darwin", "/Users/jo", {}}, "/Users/jo/Library/Application Support/KeyGrant/sluice.json"},
      {"Linux with $XDG_CONFIG_HOME", posix({{"XDG_CONFIG_HOME", "/home/jo/.cfg"}}), "/home/jo/.cfg/keygrant/sluice.json"},
      {"Linux without it", posix({}), "/home/jo/.config/keygrant/sluice.json"},
      {"Linux with it empty", posix({{"XDG_CONFIG_HOME", ""}}), "/home/jo/.config/keygrant/sluice.json"},
      {"Linux with it relative", posix({{"XDG_CONFIG_HOME", ".cfg"}}), "/home/jo/.config/keygrant/sluice.json"},
  };
  for (const Case& c : cases) EXPECT_EQ(keygrant::defaultStoragePath("sluice", c.system), c.expected) << c.name;
}

TEST(DefaultStoragePath, RefusesAtConstructionRatherThanKeepTheStateInTheWorkingDirectory) {
  // No home folder (HOME unset or empty, no passwd entry: an arbitrary UID in a
  // container), or a relative one, and nothing absolute instead: no folder of
  // this user's to keep the state in. Node's os.homedir() throws there too.
  using keygrant::SystemPaths;
  for (const SystemPaths& system : {SystemPaths{"linux", "", {}}, SystemPaths{"linux", "home/jo", {}},
                                    SystemPaths{"linux", "", {{"XDG_CONFIG_HOME", ".cfg"}}},
                                    SystemPaths{"darwin", "", {}}, SystemPaths{"darwin", "Users/jo", {}},
                                    SystemPaths{"win32", "", {}}, SystemPaths{"win32", "Users\\jo", {{"LOCALAPPDATA", "AppData"}}},
                                    SystemPaths{"freebsd", "", {}}}) {
    EXPECT_THROW(keygrant::defaultStoragePath("sluice", system), std::invalid_argument) << system.platform << " '"
                                                                                          << system.homedir << "'";
  }
  // An absolute folder of the user's own needs no home folder.
  EXPECT_EQ(keygrant::defaultStoragePath("sluice", SystemPaths{"linux", "", {{"XDG_CONFIG_HOME", "/srv/cfg"}}}),
            "/srv/cfg/keygrant/sluice.json");
  EXPECT_EQ(keygrant::defaultStoragePath("sluice", SystemPaths{"win32", "", {{"LOCALAPPDATA", "D:\\Local"}}}),
            "D:\\Local\\KeyGrant\\sluice.json");
}

TEST(DefaultStoragePath, IsTheRunningSystemsOwn) {
  const std::string path = keygrant::defaultStoragePath("sluice");
  EXPECT_TRUE(path.size() > std::string("sluice.json").size());
  EXPECT_EQ(keygrant::detail::baseOf(path), "sluice.json");
#ifdef __linux__
  EXPECT_NE(path.find("keygrant"), std::string::npos);
#endif
}

// --- writes ----------------------------------------------------------------------------

TEST(FileStorageWrites, RenameATempFileOverTheStateAndLeaveNoTempBehind) {
  TempDir dir;
  FileStorage storage(dir.file("sluice.json"));
  storage.write(keyed("K", 1));
  storage.write(keyed("K", 2));
  EXPECT_EQ(json::parse(slurp(dir.file("sluice.json"))), json::parse(R"({"key":"K","lastSeen":2})"));
  EXPECT_EQ(dir.names(), std::vector<std::string>{"sluice.json"});
}

TEST(FileStorageWrites, ReplaceTheStateRatherThanWriteIntoIt) {
  // A write in place changes the one file every name for it shares; a rename
  // puts a new file under the name and leaves the old one whole.
  TempDir dir;
  FileStorage storage(dir.file("sluice.json"));
  storage.write(keyed("K", 1));
  fs::create_hard_link(dir.file("sluice.json"), dir.file("old-state"));
  storage.write(keyed("K", 2));
  EXPECT_EQ(json::parse(slurp(dir.file("old-state"))), json::parse(R"({"key":"K","lastSeen":1})"));
  EXPECT_EQ(json::parse(slurp(dir.file("sluice.json"))), json::parse(R"({"key":"K","lastSeen":2})"));
}

TEST(FileStorageWrites, ClearTheTempsCrashesLeftAndNotAnotherInstancesWriteInFlight) {
#ifdef _WIN32
  GTEST_SKIP() << "sets mtimes with utimensat";
#else
  TempDir dir;
  const std::int64_t now = keygrant::SystemClock().now();
  spit(dir.file("sluice.json.1.1.crashed.tmp"), R"({"key":"CRASHED"})");
  ASSERT_TRUE(age(dir.file("sluice.json.1.1.crashed.tmp"), now - 2 * keygrant::kStaleTempMs));
  spit(dir.file("sluice.json.2.2.inflight.tmp"), R"({"key":"IN)");
  // Another product's temp in the same folder is not this file's to clear.
  spit(dir.file("maindeck.json.3.3.other.tmp"), "{}");
  ASSERT_TRUE(age(dir.file("maindeck.json.3.3.other.tmp"), now - 2 * keygrant::kStaleTempMs));
  // Either side of the cutoff, five seconds each way.
  spit(dir.file("sluice.json.4.4.younger.tmp"), "{}");
  ASSERT_TRUE(age(dir.file("sluice.json.4.4.younger.tmp"), now - keygrant::kStaleTempMs + 5'000));
  spit(dir.file("sluice.json.5.5.older.tmp"), "{}");
  ASSERT_TRUE(age(dir.file("sluice.json.5.5.older.tmp"), now - keygrant::kStaleTempMs - 5'000));

  FileStorage(dir.file("sluice.json")).write(keyed("K"));
  EXPECT_EQ(dir.names(), (std::vector<std::string>{"maindeck.json.3.3.other.tmp", "sluice.json",
                                                   "sluice.json.2.2.inflight.tmp", "sluice.json.4.4.younger.tmp"}));
  EXPECT_EQ(keygrant::kStaleTempMs, 60'000);
#endif
}

TEST(FileStorageWrites, ClearATempAgedExactlyTheCutoffAndKeepOneAMillisecondYounger) {
  const std::int64_t now = 1'757'000'000'000;
  const std::string edge = memoryState() + ".1.1.edge.tmp";
  const std::string inside = memoryState() + ".2.2.inside.tmp";
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), kSaved}, {edge, "{}"}, {inside, "{}"}});
  memory->setMtime(edge, now - keygrant::kStaleTempMs);
  memory->setMtime(inside, now - keygrant::kStaleTempMs + 1);
  FileStorageOptions options;
  options.fs = memory;
  options.sleep = NoSleep().fn();
  // An init-capture: Apple Clang calls a plain `[now]` an unused capture (`now` is a constant), and
  // -Werror makes that a build failure on macOS.
  options.now = [at = now] { return at; };
  FileStorage(memoryState(), options).write(keyed("K"));
  std::vector<std::string> names;
  for (const auto& [path, text] : memory->files()) names.push_back(path);
  EXPECT_EQ(names, (std::vector<std::string>{memoryState(), inside}));
}

TEST(FileStorageWrites, TryABusyRenameAgain) {
  for (const char* code : {"EPERM", "EBUSY", "EACCES"}) {
    auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), kSaved}});
    memory->failNext(Op::Rename, {code, code});
    NoSleep sleep;
    storageOver(memory, sleep)->write(keyed("K", 2));
    EXPECT_EQ(json::parse(*memory->file(memoryState())), json::parse(R"({"key":"K","lastSeen":2})")) << code;
    EXPECT_EQ(sleep.slept->size(), 2u) << code;
    EXPECT_EQ(memory->files().size(), 1u) << code;
  }
}

TEST(FileStorageWrites, GiveUpOnARenameThatStaysBusyKeepingTheStateAndRemovingTheTemp) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), kSaved}});
  memory->failAlways(Op::Rename, "EBUSY");
  NoSleep sleep;
  try {
    storageOver(memory, sleep)->write(keyed("K"));
    FAIL() << "expected the write to fail";
  } catch (const IoError& error) {
    EXPECT_EQ(error.code(), "EBUSY");
  }
  EXPECT_GE(sleep.total(), 1'000);
  EXPECT_LE(sleep.total(), 2'000);
  EXPECT_EQ(memory->files(), (std::map<std::string, std::string>{{memoryState(), kSaved}}));
}

TEST(FileStorageWrites, DoNotTryAgainARenameThatFailedOtherThanBusy) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), kSaved}});
  memory->failNext(Op::Rename, {"ENOSPC"});
  NoSleep sleep;
  EXPECT_THROW(storageOver(memory, sleep)->write(keyed("K")), IoError);
  EXPECT_TRUE(sleep.slept->empty());
  EXPECT_EQ(memory->files(), (std::map<std::string, std::string>{{memoryState(), kSaved}}));
}

TEST(FileStorageWrites, FlushTheTempAndCloseItBeforeTheRenameAndTheFolderAfter) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), kSaved}});
  storageOver(memory)->write(keyed("K"));
  std::vector<std::string> steps;
  for (const std::string& op : memory->ops()) {
    const std::string name = op.substr(0, op.find(' '));
    if (name != "readdir" && name != "mtimeMs" && name != "mkdir") steps.push_back(name);
  }
  EXPECT_EQ(steps, (std::vector<std::string>{"create", "write", "sync", "close", "rename", "syncDir"}));
}

TEST(FileStorageWrites, FailAWriteWhoseTempCannotBeFlushedTheStateAsItWasAndNoTemp) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), kSaved}});
  memory->failNext(Op::Sync, {"EIO"});
  EXPECT_THROW(storageOver(memory)->write(keyed("K")), IoError);
  EXPECT_EQ(memory->files(), (std::map<std::string, std::string>{{memoryState(), kSaved}}));
}

TEST(FileStorageWrites, FailNothingWhenTheFolderCannotBeFlushed) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), kSaved}});
  memory->failNext(Op::SyncDir, {"EINVAL"});
  storageOver(memory)->write(keyed("K"));
  EXPECT_EQ(memory->files(), (std::map<std::string, std::string>{{memoryState(), R"({"key":"K"})"}}));
}

TEST(FileStorageWrites, LeaveTheStateAndNoTempWhenAWriteFails) {
  // The state path is a folder: the rename over it fails.
  TempDir dir;
  fs::create_directory(dir.file("sluice.json"));
  FileStorageOptions options;
  options.sleep = NoSleep().fn();
  EXPECT_THROW(FileStorage(dir.file("sluice.json"), options).write(keyed("K")), IoError);
  EXPECT_EQ(dir.names(), std::vector<std::string>{"sluice.json"});
}

TEST(FileStorageWrites, CreateTheFileAndItsFoldersForItsOwnerOnly) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX permissions";
#else
  TempDir dir;
  FileStorage(dir.file("keygrant/nested/sluice.json")).write(keyed("K"));
  struct stat info {};
  ASSERT_EQ(stat(dir.file("keygrant/nested/sluice.json").c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0600u);
  // Every folder it had to make, as well as the file.
  for (const char* folder : {"keygrant", "keygrant/nested"}) {
    ASSERT_EQ(stat(dir.file(folder).c_str(), &info), 0) << folder;
    EXPECT_TRUE(S_ISDIR(info.st_mode)) << folder;
    EXPECT_EQ(info.st_mode & 0777, 0700u) << folder;
  }
#endif
}

// --- reads -------------------------------------------------------------------------------

TEST(FileStorageReads, AreNoStateWhenTheFileDoesNotExistThenRoundTripTheState) {
  TempDir dir;
  FileStorage storage(dir.file("nested/deeper/license.json"));
  EXPECT_EQ(storage.read(false), std::nullopt);
  StoredState state = keyed("K", 42);
  state.activationId = "a";
  storage.write(state);
  EXPECT_EQ(storage.read(false), state);
}

TEST(FileStorageReads, ReadAndSaveAStateWhoseUnknownFieldNestsDeepNeverACrash) {
  // A field nested tens of thousands deep (a damaged file, or one made so):
  // writing it back would recurse as deep, so it is dropped, not kept, and the
  // licence in the same file reads and saves as ever.
  for (const std::size_t depth : kDeepNesting) {
    SCOPED_TRACE(depth);
    TempDir dir;
    const std::string path = dir.file("sluice.json");
    spit(path, R"({"key":"K","lastSeen":5,"future":)" + nestedArrays(depth) + R"(,"note":[[1,{"a":[]}]]})");
    FileStorage storage(path);
    for (const bool readOnly : {true, false}) {
      const std::optional<StoredState> state = storage.read(readOnly);
      ASSERT_TRUE(state);
      EXPECT_EQ(state->key, "K");
      EXPECT_EQ(state->otherFields.count("future"), 0u);
      EXPECT_EQ(state->otherFields.count("note"), 1u);
    }
    storage.update([](const StoredState& latest) {
      StoredState next = latest;
      next.lastSeen = 6;
      return next;
    });
    const json saved = json::parse(slurp(path));
    EXPECT_EQ(saved["key"], "K");
    EXPECT_EQ(saved["lastSeen"], 6);
    EXPECT_EQ(saved["note"], json::parse(R"([[1,{"a":[]}]])"));
    EXPECT_FALSE(saved.contains("future"));
  }
}

TEST(FileStorageReads, ReadTheStateItHadWhenAWriteCrashedBeforeItsRename) {
  TempDir dir;
  FileStorage(dir.file("sluice.json")).write(keyed("OLD", 1));
  spit(dir.file("sluice.json.123.456.abc.tmp"), R"({"key":"NE)");
  EXPECT_EQ(FileStorage(dir.file("sluice.json")).read(false), keyed("OLD", 1));
}

TEST(FileStorageReads, ReadTheNewestWholeTempByItsTimeWhenTheStateIsMissing) {
  // Named so that their names sort the opposite way to their times: only the times tell.
  const std::string newer = memoryState() + ".1.1.a.tmp";
  const std::string older = memoryState() + ".9.9.z.tmp";
  const std::string torn = memoryState() + ".5.5.m.tmp";
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{
      {newer, R"({"key":"NEW"})"}, {older, R"({"key":"OLD"})"}, {torn, R"({"key":"TOR)"}});
  memory->setMtime(older, NOW - 30'000);
  memory->setMtime(newer, NOW - 20'000);
  memory->setMtime(torn, NOW - 10'000);
  const auto storage = storageOver(memory);
  EXPECT_EQ(storage->read(false), keyed("NEW"));
  memory->rm(newer);
  EXPECT_EQ(storage->read(false), keyed("OLD"));
  memory->rm(older);
  EXPECT_EQ(storage->read(false), std::nullopt);
}

TEST(FileStorageReads, SetAFileThatDoesNotParseAsideAndWriteAnEmptyStateInItsPlace) {
  TempDir dir;
  spit(dir.file("sluice.json"), R"({"key":"SLUICE-AA)");
  spit(dir.file("sluice.json.1.1.a.tmp"), R"({"key":"FROM-A-TEMP"})");
  FileStorage storage(dir.file("sluice.json"));
  EXPECT_EQ(storage.read(false), std::nullopt);
  std::string aside;
  for (const std::string& name : dir.names()) {
    if (name.rfind("sluice.json.", 0) == 0 && name.size() > 20 && name.substr(name.size() - 8) == ".corrupt") aside = name;
  }
  ASSERT_FALSE(aside.empty());
  EXPECT_EQ(slurp(dir.file(aside)), R"({"key":"SLUICE-AA)");
  // An empty state in its place, so no later read takes the temp instead.
  EXPECT_EQ(slurp(dir.file("sluice.json")), "{}");
  EXPECT_EQ(storage.read(false), StoredState{});
}

TEST(FileStorageReads, KeepOnlyTheNewestSetAsideFilesByTheTimeInTheirNames) {
  const auto aside = [](const char* at) { return memoryState() + "." + at + ".corrupt"; };
  // 900 sorts after 3000 as text: only the numbers tell. tunnel.json. is as long as sluice.json.
  const std::string otherProduct = keygrant::detail::joinPath(memoryDir(), "tunnel.json.100.corrupt");
  const std::string notAside = memoryState() + ".notes.corrupt";
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{
      {memoryState(), "{\"bad"}, {aside("900"), "x"}, {aside("1000"), "x"}, {aside("2000"), "x"},
      {aside("3000"), "x"}, {otherProduct, "x"}, {notAside, "x"}});
  EXPECT_EQ(storageOver(memory)->read(false), std::nullopt);
  const auto files = memory->files();
  int kept = 0;
  for (const auto& [path, text] : files) {
    (void)text;
    const std::string name = keygrant::detail::baseOf(path);
    const bool timed = name.rfind("sluice.json.", 0) == 0 && name.find(".corrupt") != std::string::npos &&
                       name.find("notes") == std::string::npos;
    kept += timed ? 1 : 0;
  }
  EXPECT_EQ(kept, keygrant::kKeepCorrupt);
  EXPECT_EQ(files.count(aside("3000")), 1u);
  EXPECT_EQ(files.count(aside("2000")), 1u);
  EXPECT_EQ(files.count(aside("1000")), 0u);
  EXPECT_EQ(files.count(aside("900")), 0u);
  EXPECT_EQ(files.count(otherProduct), 1u);
  EXPECT_EQ(files.count(notAside), 1u);
}

TEST(FileStorageReads, ReadOnlyRepairsNothingAFileThatDoesNotParseIsNoStateAndStays) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), "{\"bad"}});
  EXPECT_EQ(storageOver(memory)->read(true), std::nullopt);
  EXPECT_EQ(memory->files(), (std::map<std::string, std::string>{{memoryState(), "{\"bad"}}));
  EXPECT_TRUE(memory->changes().empty());
}

TEST(FileStorageReads, ReadOnlyReadsALeftoverTempAndWritesNothing) {
  const std::string temp = memoryState() + ".1.1.a.tmp";
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{temp, R"({"key":"FROM-A-TEMP"})"}});
  EXPECT_EQ(storageOver(memory)->read(true), keyed("FROM-A-TEMP"));
  EXPECT_TRUE(memory->changes().empty());
  EXPECT_EQ(memory->files().size(), 1u);
}

TEST(FileStorageReads, TryAReadThatFailedAgain) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), kSaved}});
  memory->failNext(Op::ReadFile, {"EBUSY"});
  NoSleep sleep;
  EXPECT_EQ(storageOver(memory, sleep)->read(false), keygrant::parseState(kSaved));
  EXPECT_EQ(sleep.slept->size(), 1u);
}

TEST(FileStorageReads, ThrowOnAReadThatStillFailsAfterAboutASecondRatherThanReadNoStateOrATemp) {
  for (const char* code : {"EBUSY", "EACCES", "EIO"}) {
    auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{
        {memoryState(), kSaved}, {memoryState() + ".1.1.a.tmp", R"({"key":"FROM-A-TEMP"})"}});
    memory->failAlways(Op::ReadFile, code);
    NoSleep sleep;
    try {
      storageOver(memory, sleep)->read(false);
      FAIL() << "expected the read to throw " << code;
    } catch (const IoError& error) {
      EXPECT_EQ(error.code(), code);
    }
    EXPECT_GE(sleep.total(), 500) << code;
    EXPECT_LE(sleep.total(), 1'500) << code;
    EXPECT_TRUE(memory->changes().empty()) << code;
  }
}

TEST(FileStorageReads, DoNotWaitOnAMissingFile) {
  NoSleep sleep;
  EXPECT_EQ(storageOver(std::make_shared<MemoryFs>(), sleep)->read(false), std::nullopt);
  EXPECT_TRUE(sleep.slept->empty());
}

TEST(FileStorageReads, ThrowWritingNothingWhenAFileThatDoesNotParseCannotBeSetAside) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), R"({"key":"SLUICE-AA)"}});
  memory->failAlways(Op::Rename, "EBUSY");
  EXPECT_THROW(storageOver(memory)->read(false), IoError);
  EXPECT_EQ(memory->files(), (std::map<std::string, std::string>{{memoryState(), R"({"key":"SLUICE-AA)"}}));
}

// --- the state an older SDK kept ---------------------------------------------------------

TEST(FileStorageMigration, IsCopiedOnceAndReadFromTheNewPathAfter) {
  TempDir dir;
  const std::string legacy = dir.file("app/.keygrant/sluice.json");
  fs::create_directories(fs::path(legacy).parent_path());
  spit(legacy, kSaved);
  FileStorageOptions options;
  options.migrateFrom = legacy;
  FileStorage storage(dir.file("user/sluice.json"), options);
  EXPECT_EQ(storage.read(false), keygrant::parseState(kSaved));
  EXPECT_EQ(json::parse(slurp(dir.file("user/sluice.json"))), json::parse(kSaved));
  EXPECT_EQ(slurp(legacy), kSaved);
  // From then on only the new path.
  spit(legacy, R"({"key":"CHANGED"})");
  EXPECT_EQ(storage.read(false), keygrant::parseState(kSaved));
}

TEST(FileStorageMigration, IsIgnoredWhenTheNewPathHasAState) {
  TempDir dir;
  const std::string legacy = dir.file("legacy.json");
  spit(legacy, kSaved);
  FileStorage(dir.file("user/sluice.json")).write(keyed("NEWER"));
  FileStorageOptions options;
  options.migrateFrom = legacy;
  EXPECT_EQ(FileStorage(dir.file("user/sluice.json"), options).read(false), keyed("NEWER"));
}

TEST(FileStorageMigration, IsNoStateAndNothingWrittenWhenMissingOrUnparseable) {
  TempDir dir;
  FileStorageOptions options;
  options.migrateFrom = dir.file("legacy.json");
  EXPECT_EQ(FileStorage(dir.file("user/sluice.json"), options).read(false), std::nullopt);
  spit(dir.file("legacy.json"), R"({"key":"SLU)");
  EXPECT_EQ(FileStorage(dir.file("user/sluice.json"), options).read(false), std::nullopt);
  EXPECT_EQ(dir.names(), std::vector<std::string>{"legacy.json"});
}

TEST(FileStorageMigration, IsStillReadWhenItCannotBeCopiedAndCopiedAtALaterRead) {
  const std::string legacy = keygrant::detail::joinPath(memoryDir() + "-legacy", "sluice.json");
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{legacy, kSaved}});
  memory->failNext(Op::Write, {"ENOSPC"});
  const auto storage = storageOver(memory, {}, legacy);
  EXPECT_EQ(storage->read(false), keygrant::parseState(kSaved));
  EXPECT_EQ(memory->file(memoryState()), std::nullopt);
  EXPECT_EQ(storage->read(false), keygrant::parseState(kSaved));
  ASSERT_TRUE(memory->file(memoryState()));
  EXPECT_EQ(json::parse(*memory->file(memoryState())), json::parse(kSaved));
}

TEST(FileStorageMigration, IsReadNotCopiedByAReadOnlyRead) {
  const std::string legacy = keygrant::detail::joinPath(memoryDir() + "-legacy", "sluice.json");
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{legacy, kSaved}});
  EXPECT_EQ(storageOver(memory, {}, legacy)->read(true), keygrant::parseState(kSaved));
  EXPECT_TRUE(memory->changes().empty());
}

// --- revisions -------------------------------------------------------------------------------

TEST(FileStorageUpdate, StampsEverySaveWithTheNextRevision) {
  auto memory = std::make_shared<MemoryFs>();
  const auto storage = storageOver(memory);
  storage->update([](const StoredState& latest) {
    StoredState next = latest;
    next.key = "K";
    return next;
  });
  EXPECT_EQ(json::parse(*memory->file(memoryState())), json::parse(R"({"key":"K","rev":1})"));
  storage->update([](const StoredState& latest) {
    StoredState next = latest;
    next.checkedMajor = 2;
    return next;
  });
  EXPECT_EQ(json::parse(*memory->file(memoryState())), json::parse(R"({"key":"K","checkedMajor":2,"rev":2})"));
}

TEST(FileStorageUpdate, FailsWithConflictAfterAFewTriesWhenTheStateKeepsChangingKeepingTheOthers) {
  auto memory = std::make_shared<MemoryFs>(std::map<std::string, std::string>{{memoryState(), R"({"key":"K","rev":1})"}});
  int other = 1;
  // Another process saves every time this one has read and is about to write.
  memory->before(Op::Create, [&] {
    other += 1;
    memory->put(memoryState(), R"({"key":"OTHER","rev":)" + std::to_string(other) + "}");
  });
  try {
    storageOver(memory)->update([](const StoredState& latest) {
      StoredState next = latest;
      next.keptMajor = 2;
      return next;
    });
    FAIL() << "expected a conflict";
  } catch (const IoError& error) {
    EXPECT_EQ(error.code(), keygrant::kConflict);
  }
  EXPECT_EQ(memory->count(Op::Create), keygrant::kUpdateAttempts);
  EXPECT_EQ(keygrant::kUpdateAttempts, 5);
  EXPECT_EQ(json::parse(*memory->file(memoryState())), json::parse(R"({"key":"OTHER","rev":)" + std::to_string(other) + "}"));
  EXPECT_EQ(memory->files().size(), 1u);
}

}  // namespace
}  // namespace kgtest
