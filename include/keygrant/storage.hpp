// The SDK's default state file: where it lives, and how it is read and written so that neither a
// crash nor a busy disk ever costs a customer their licence.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "keygrant/adapters.hpp"

namespace keygrant {

/// How many set-aside `.corrupt` files of a state file are kept: the newest.
inline constexpr int kKeepCorrupt = 3;
/// How old a temp file must be to be a crash's leftover rather than another process's write in flight.
inline constexpr std::int64_t kStaleTempMs = 60'000;
/// How many times a save is merged onto a state another process changed under it before it fails.
inline constexpr int kUpdateAttempts = 5;
/// The code of the IoError a save fails with when the state kept changing under it.
inline constexpr const char* kConflict = "ECONFLICT";

/** What the default path is worked out from (the running system, or a stand-in in tests). */
struct SystemPaths {
  /// Node's name for the platform: "win32", "darwin", "linux", ...
  std::string platform;
  /// The user's home folder.
  std::string homedir;
  /// The environment variables that matter: LOCALAPPDATA, XDG_CONFIG_HOME.
  std::map<std::string, std::string> env;

  /// The running system's.
  static SystemPaths current();
};

/**
 * Where the default storage keeps a product's state: a folder of this user's on
 * THIS machine, never the working directory and never a folder that roams
 * between machines:
 *  - Windows: `%LOCALAPPDATA%\KeyGrant`, else `<home>\AppData\Local\KeyGrant`
 *  - macOS: `~/Library/Application Support/KeyGrant`
 *  - Linux and others: `$XDG_CONFIG_HOME/keygrant`, else `~/.config/keygrant`
 * An empty or relative variable is taken as unset. Throws std::invalid_argument
 * when no absolute folder is left (no home folder, or a relative one): a License
 * built on the default storage then fails at construction, as the reference's
 * does, rather than keep the state in the working directory.
 */
std::string defaultStoragePath(const std::string& product, const SystemPaths& system);
std::string defaultStoragePath(const std::string& product);

/** A new file being written: what is written is flushed to the disk by sync(). */
class StorageFile {
 public:
  virtual ~StorageFile() = default;
  virtual void write(const std::string& data) = 0;
  virtual void sync() = 0;
  virtual void close() = 0;
};

/**
 * The file operations under FileStorage: the real ones, or stand-ins in tests.
 * Every failure is an IoError with an errno-style code ("ENOENT" for a file
 * that is not there).
 */
class StorageFs {
 public:
  virtual ~StorageFs() = default;
  virtual std::string readFile(const std::string& path) = 0;
  /// A new file to write, failing ("EEXIST") when there is one by that name.
  virtual std::unique_ptr<StorageFile> create(const std::string& path) = 0;
  /// Rename `from` over `to`, replacing it.
  virtual void rename(const std::string& from, const std::string& to) = 0;
  /// Remove a file; one that is not there is no failure.
  virtual void rm(const std::string& path) = 0;
  /// The names in a folder.
  virtual std::vector<std::string> readdir(const std::string& dir) = 0;
  /// When a file's content was last written (ms since the epoch).
  virtual std::int64_t mtimeMs(const std::string& path) = 0;
  /// Make a folder and its parents.
  virtual void mkdir(const std::string& dir) = 0;
  /// Flush a folder's entries (a rename in it) to the disk. A no-op on Windows.
  virtual void syncDir(const std::string& dir) = 0;
};

/** The real file system (POSIX, or Win32 with UTF-8 paths). */
std::shared_ptr<StorageFs> systemStorageFs();

/** Options for FileStorage. */
struct FileStorageOptions {
  /// The file operations; default the real file system.
  std::shared_ptr<StorageFs> fs;
  /// A state an older SDK kept elsewhere, copied to the path once when it has none.
  std::optional<std::string> migrateFrom;
  /// How a retry waits; default std::this_thread::sleep_for.
  std::function<void(std::chrono::milliseconds)> sleep;
  /// The wall clock in ms, for temp names, stale temps and set-aside names; default the system's.
  std::function<std::int64_t()> now;
};

/**
 * JSON file storage, read and written as the Electron SDK does:
 *  - Read: only a MISSING file is "no state" (then the newest whole temp file a
 *    crash left, else `migrateFrom`'s state, copied once). Any other failure is
 *    tried again for about a second and then throws. A file that does not parse
 *    is set aside as `<file>.<ms>.corrupt` (the newest kKeepCorrupt kept) and an
 *    empty state written in its place. A read-only read does neither.
 *  - Written atomically: a temp file beside it, flushed to the disk, renamed
 *    over it (tried again for about a second and a half while the file is busy),
 *    the folder flushed after on POSIX; stale temps cleared.
 *  - update() stamps every save with the next revision (`rev`), checks just
 *    before the rename that the file's revision is still the one it read, and
 *    merges again onto another process's newer state (kUpdateAttempts tries,
 *    then IoError kConflict).
 * Files are created readable by their owner only (0600) on POSIX.
 */
class FileStorage final : public StorageAdapter {
 public:
  explicit FileStorage(std::string path, FileStorageOptions options = {});

  std::optional<StoredState> read(bool readOnly) override;
  void write(const StoredState& state) override;
  bool update(const std::function<StoredState(const StoredState&)>& merge) override;

  const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
  std::string dir_;
  std::string prefix_;
  FileStorageOptions options_;
};

}  // namespace keygrant
