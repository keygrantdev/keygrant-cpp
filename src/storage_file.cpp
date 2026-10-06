// FileStorage: the reference's createFileStorage (sdk/src/storage.ts) and the
// file handling under it (sdk/src/storage-fs.ts), step for step.
#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "json_util.hpp"
#include "keygrant/storage.hpp"
#include "paths.hpp"
#include "state_json.hpp"
#include "sys.hpp"

namespace keygrant {

namespace {

using Milliseconds = std::chrono::milliseconds;
using Sleep = std::function<void(Milliseconds)>;

using Backoff = std::array<Milliseconds, 5>;
/// Waits between tries of a read that failed other than for a missing file: about a second in all.
constexpr Backoff kReadBackoff = {Milliseconds{50}, Milliseconds{100}, Milliseconds{200}, Milliseconds{300},
                                  Milliseconds{350}};
/// Waits between tries of a rename a busy file refused: about a second and a half in all.
constexpr Backoff kRenameBackoff = {Milliseconds{50}, Milliseconds{100}, Milliseconds{200}, Milliseconds{400},
                                    Milliseconds{800}};

/// Errors a busy file gives a rename on Windows (an antivirus scan, an indexer, a backup), worth another try.
bool busy(const std::optional<std::string>& code) {
  return code && (*code == "EBUSY" || *code == "EPERM" || *code == "EACCES");
}
bool missing(const std::optional<std::string>& code) { return code && *code == "ENOENT"; }
bool notMissing(const std::optional<std::string>& code) { return !missing(code); }

/** `work`, tried again after each wait while its error is worth another try; the last error otherwise. */
template <class Work>
auto retrying(Work work, const Backoff& backoff, bool (*worthAgain)(const std::optional<std::string>&),
              const Sleep& sleep) -> decltype(work()) {
  for (const Milliseconds wait : backoff) {
    try {
      return work();
    } catch (const IoError& error) {
      if (!worthAgain(error.code())) throw;
    } catch (...) {
      if (!worthAgain(std::nullopt)) throw;
    }
    sleep(wait);
  }
  return work();
}

bool startsWith(const std::string& text, const std::string& prefix) { return text.rfind(prefix, 0) == 0; }

bool endsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/** The state a file's text holds: a JSON object; none for any other JSON. Throws when it is not JSON. */
std::optional<StoredState> stateOf(const std::string& text) {
  const detail::json parsed = detail::parseJsonText(text);
  if (parsed.is_discarded()) throw std::invalid_argument("not JSON");
  if (!parsed.is_object()) return std::nullopt;
  return detail::stateFromJson(parsed);
}

/** The revision a file's text carries: none when missing or unstamped; garbage (equal to nothing) when unparseable. */
struct Rev {
  enum class Kind { None, Value, Garbage };
  Kind kind = Kind::None;
  std::int64_t value = 0;

  static Rev of(const std::optional<std::int64_t>& rev) { return rev ? Rev{Kind::Value, *rev} : Rev{}; }
  bool same(const Rev& other) const {
    if (kind == Kind::Garbage || other.kind == Kind::Garbage || kind != other.kind) return false;
    return kind == Kind::None || value == other.value;
  }
};

Rev revOf(const std::optional<std::string>& text) {
  if (!text) return Rev{};
  try {
    const std::optional<StoredState> state = stateOf(*text);
    return state ? Rev::of(state->rev) : Rev{};
  } catch (...) {
    return Rev{Rev::Kind::Garbage, 0};
  }
}

/** A file's text, tried again for about a second on any failure but a missing file; none when it is missing. */
std::optional<std::string> readText(StorageFs& fs, const std::string& path, const Sleep& sleep) {
  try {
    return retrying([&] { return fs.readFile(path); }, kReadBackoff, notMissing, sleep);
  } catch (const IoError& error) {
    if (error.code() == "ENOENT") return std::nullopt;
    throw;
  }
}

std::vector<std::string> tempsOf(StorageFs& fs, const std::string& dir, const std::string& prefix) {
  std::vector<std::string> temps;
  for (const std::string& name : fs.readdir(dir)) {
    if (startsWith(name, prefix) && endsWith(name, ".tmp")) temps.push_back(name);
  }
  return temps;
}

/** The newest temp file of a state file that parses, when the file itself is missing. Never throws. */
std::optional<StoredState> leftover(StorageFs& fs, const std::string& dir, const std::string& prefix) {
  try {
    std::vector<std::pair<std::string, std::int64_t>> dated;
    for (const std::string& name : tempsOf(fs, dir, prefix)) dated.emplace_back(name, fs.mtimeMs(detail::joinPath(dir, name)));
    std::stable_sort(dated.begin(), dated.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [name, at] : dated) {
      (void)at;
      try {
        return stateOf(fs.readFile(detail::joinPath(dir, name)));
      } catch (...) {
        // Torn: the next older one.
      }
    }
  } catch (...) {
    // No temps to read.
  }
  return std::nullopt;
}

/** Remove a storage file's temp files older than kStaleTempMs. Best-effort. */
void clearStale(StorageFs& fs, const std::string& dir, const std::string& prefix, std::int64_t now) {
  try {
    for (const std::string& name : tempsOf(fs, dir, prefix)) {
      const std::string path = detail::joinPath(dir, name);
      std::int64_t at = now;
      try {
        at = fs.mtimeMs(path);
      } catch (...) {
        at = now;
      }
      if (now - at >= kStaleTempMs) {
        try {
          fs.rm(path);
        } catch (...) {
          // Left for the next write.
        }
      }
    }
  } catch (...) {
    // Left for the next write.
  }
}

/** Whether digit string `a` is a larger number than `b`. */
bool numericallyAfter(const std::string& a, const std::string& b) {
  const std::string x = a.substr(std::min(a.find_first_not_of('0'), a.size()));
  const std::string y = b.substr(std::min(b.find_first_not_of('0'), b.size()));
  return x.size() != y.size() ? x.size() > y.size() : x > y;
}

/** Remove all but the newest kKeepCorrupt set-aside files, newest by the time in their name. Best-effort. */
void pruneCorrupt(StorageFs& fs, const std::string& dir, const std::string& prefix) {
  try {
    std::vector<std::pair<std::string, std::string>> aside;
    for (const std::string& name : fs.readdir(dir)) {
      if (!startsWith(name, prefix) || !endsWith(name, ".corrupt")) continue;
      const std::string at = name.substr(prefix.size(), name.size() - prefix.size() - 8);
      const bool digits = !at.empty() && std::all_of(at.begin(), at.end(), [](char c) { return c >= '0' && c <= '9'; });
      if (digits) aside.emplace_back(name, at);
    }
    std::stable_sort(aside.begin(), aside.end(), [](const auto& a, const auto& b) { return numericallyAfter(a.second, b.second); });
    for (std::size_t i = static_cast<std::size_t>(kKeepCorrupt); i < aside.size(); ++i) {
      try {
        fs.rm(detail::joinPath(dir, aside[i].first));
      } catch (...) {
        // Kept until the next one is set aside.
      }
    }
  } catch (...) {
    // Kept until the next one is set aside.
  }
}

/**
 * A state written whole to a new temp file in `dir` and flushed to the disk
 * before it is closed: a rename of it after a power cut finds it whole. Removed
 * when it fails.
 */
std::string writeTemp(StorageFs& fs, const std::string& dir, const std::string& prefix, const StoredState& state,
                      std::int64_t now) {
  fs.mkdir(dir);
  const std::string temp = detail::joinPath(
      dir, prefix + std::to_string(detail::processId()) + "." + std::to_string(now) + "." + detail::randomSuffix() + ".tmp");
  try {
    std::unique_ptr<StorageFile> file = fs.create(temp);
    try {
      file->write(serializeState(state));
      file->sync();
    } catch (...) {
      try {
        file->close();
      } catch (...) {
        // The write's own failure is the one to report.
      }
      throw;
    }
    file->close();
    return temp;
  } catch (...) {
    try {
      fs.rm(temp);
    } catch (...) {
      // Cleared as stale by a later write.
    }
    throw;
  }
}

}  // namespace

namespace {

/** What a load gives: the state, and the revision of the FILE it was read from (none from anywhere else). */
struct Loaded {
  std::optional<StoredState> state;
  Rev rev;
};

/** One storage file's steps, over its options. */
class Store {
 public:
  Store(const std::string& path, const std::string& dir, const std::string& prefix, const FileStorageOptions& options)
      : path_(path), dir_(dir), prefix_(prefix), options_(options), fs_(*options.fs) {}

  /** A whole state put in place, once `allowed` (when given) has not thrown, just before the rename. */
  void place(const StoredState& state, const std::function<void()>& allowed) {
    const std::string temp = writeTemp(fs_, dir_, prefix_, state, options_.now());
    try {
      const auto renameIn = [&] {
        if (allowed) allowed();
        fs_.rename(temp, path_);
        return true;
      };
      retrying(renameIn, kRenameBackoff, busy, options_.sleep);
    } catch (...) {
      try {
        fs_.rm(temp);
      } catch (...) {
        // Cleared as stale by a later write.
      }
      throw;
    }
    try {
      fs_.syncDir(dir_);
    } catch (...) {
      // The rename has landed; the folder is flushed by the system in time.
    }
    clearStale(fs_, dir_, prefix_, options_.now());
  }

  /**
   * The state. Only a MISSING file is no state: then the newest whole temp,
   * else the state an older SDK kept (copied once, not on a read-only read).
   * A file that does not parse is set aside and an empty state written in its
   * place (neither on a read-only read, which writes nothing).
   */
  Loaded load(bool readOnly) {
    const std::optional<std::string> text = readText(fs_, path_, options_.sleep);
    if (!text) {
      if (std::optional<StoredState> temp = leftover(fs_, dir_, prefix_)) return Loaded{std::move(temp), Rev{}};
      return Loaded{migrated(!readOnly), Rev{}};
    }
    try {
      std::optional<StoredState> state = stateOf(*text);
      const Rev rev = state ? Rev::of(state->rev) : Rev{};
      return Loaded{std::move(state), rev};
    } catch (const std::invalid_argument&) {
      // Not JSON: handled below.
    }
    if (readOnly) return Loaded{};
    // Kept aside before anything is written in its place; a rename that cannot
    // be made throws, as a read that cannot be made does.
    retrying(
        [&] {
          fs_.rename(path_, path_ + "." + std::to_string(options_.now()) + ".corrupt");
          return true;
        },
        kRenameBackoff, busy, options_.sleep);
    try {
      place(StoredState{}, nullptr);
    } catch (...) {
      // Written by the next save.
    }
    pruneCorrupt(fs_, dir_, prefix_);
    return Loaded{};
  }

  /** Throws kConflict when the file's revision is no longer `rev`: another process saved since it was read. */
  void unchanged(const Rev& rev) {
    std::optional<std::string> text;
    try {
      text = fs_.readFile(path_);
    } catch (const IoError& error) {
      if (error.code() != "ENOENT") throw;
    }
    if (revOf(text).same(rev)) return;
    throw IoError(kConflict, "the state was saved by another process");
  }

 private:
  /** The state an older SDK kept, copied to the path once (`copy`) and returned. Never throws. */
  std::optional<StoredState> migrated(bool copy) {
    if (!options_.migrateFrom) return std::nullopt;
    try {
      std::optional<StoredState> state = stateOf(fs_.readFile(*options_.migrateFrom));
      if (state && copy) {
        try {
          place(*state, nullptr);
        } catch (...) {
          // Read again, and copied, next time.
        }
      }
      return state;
    } catch (...) {
      return std::nullopt;
    }
  }

  const std::string& path_;
  const std::string& dir_;
  const std::string& prefix_;
  const FileStorageOptions& options_;
  StorageFs& fs_;
};

}  // namespace

FileStorage::FileStorage(std::string path, FileStorageOptions options)
    : path_(std::move(path)),
      dir_(detail::dirOf(path_)),
      prefix_(detail::baseOf(path_) + "."),
      options_(std::move(options)) {
  if (!options_.fs) options_.fs = systemStorageFs();
  if (!options_.sleep) options_.sleep = [](Milliseconds wait) { std::this_thread::sleep_for(wait); };
  if (!options_.now) options_.now = [] { return detail::wallClockMs(); };
}

std::optional<StoredState> FileStorage::read(bool readOnly) {
  return Store(path_, dir_, prefix_, options_).load(readOnly).state;
}

void FileStorage::write(const StoredState& state) { Store(path_, dir_, prefix_, options_).place(state, nullptr); }

/**
 * Saved without losing another process's save: stamped with the next revision,
 * and just before its rename the file is read again; when another process has
 * saved since, the patch is merged onto that newer state and tried again.
 */
bool FileStorage::update(const std::function<StoredState(const StoredState&)>& merge) {
  Store store(path_, dir_, prefix_, options_);
  for (int attempt = 1;; ++attempt) {
    const Loaded base = store.load(false);
    StoredState next = merge(base.state.value_or(StoredState{}));
    next.rev = (base.state && base.state->rev ? *base.state->rev : 0) + 1;
    try {
      store.place(next, [&] { store.unchanged(base.rev); });
      return true;
    } catch (const IoError& error) {
      if (error.code() != kConflict || attempt >= kUpdateAttempts) throw;
    }
  }
}

}  // namespace keygrant
