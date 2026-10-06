// Files in memory, as one or more processes see them (sdk/src/storage.test.ts
// `memoryFs`): failures queued per operation or given every time, an operation
// held until released, something another process does just before an
// operation, each file's mtime, every operation in order, and every change.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "keygrant/storage.hpp"
#include "paths.hpp"
#include "support.hpp"

namespace kgtest {

class MemoryFs final : public keygrant::StorageFs {
 public:
  enum class Op { ReadFile, Create, Write, Sync, Close, Rename, Rm, Readdir, MtimeMs, Mkdir, SyncDir };

  explicit MemoryFs(std::map<std::string, std::string> files = {}) : files_(std::move(files)) {}

  std::string readFile(const std::string& path) override {
    enter(Op::ReadFile, path, false);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = files_.find(path);
    if (it == files_.end()) throw keygrant::IoError("ENOENT", path);
    return it->second;
  }

  std::unique_ptr<keygrant::StorageFile> create(const std::string& path) override {
    enter(Op::Create, path, true);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (files_.count(path) != 0) throw keygrant::IoError("EEXIST", path);
      files_[path] = "";
    }
    return std::make_unique<File>(*this, path);
  }

  void rename(const std::string& from, const std::string& to) override {
    enter(Op::Rename, from + " " + to, true);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = files_.find(from);
    if (it == files_.end()) throw keygrant::IoError("ENOENT", from);
    files_[to] = it->second;
    files_.erase(from);
  }

  void rm(const std::string& path) override {
    enter(Op::Rm, path, true);
    std::lock_guard<std::mutex> lock(mutex_);
    files_.erase(path);
  }

  std::vector<std::string> readdir(const std::string& dir) override {
    enter(Op::Readdir, dir, false);
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& [path, text] : files_) {
      (void)text;
      if (keygrant::detail::dirOf(path) == dir) names.push_back(keygrant::detail::baseOf(path));
    }
    return names;
  }

  std::int64_t mtimeMs(const std::string& path) override {
    enter(Op::MtimeMs, path, false);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = mtimes_.find(path);
    return it != mtimes_.end() ? it->second : keygrant::SystemClock().now();
  }

  void mkdir(const std::string& dir) override { enter(Op::Mkdir, dir, false); }

  void syncDir(const std::string& dir) override { enter(Op::SyncDir, dir, false); }

  // --- what a test sees and does -------------------------------------------------

  std::map<std::string, std::string> files() {
    std::lock_guard<std::mutex> lock(mutex_);
    return files_;
  }
  std::optional<std::string> file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = files_.find(path);
    return it == files_.end() ? std::nullopt : std::optional<std::string>(it->second);
  }
  void put(const std::string& path, std::string text) {
    std::lock_guard<std::mutex> lock(mutex_);
    files_[path] = std::move(text);
  }
  void setMtime(const std::string& path, std::int64_t ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    mtimes_[path] = ms;
  }
  std::vector<std::string> ops() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ops_;
  }
  std::vector<std::string> changes() {
    std::lock_guard<std::mutex> lock(mutex_);
    return changes_;
  }
  /// How many times an operation of this kind ran (without failing first).
  int count(Op op) {
    std::lock_guard<std::mutex> lock(mutex_);
    int n = 0;
    for (const std::string& line : ops_) n += line.rfind(name(op) + " ", 0) == 0 ? 1 : 0;
    return n;
  }
  void failNext(Op op, std::vector<std::string> codes) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::string& code : codes) queued_[op].push_back(std::move(code));
  }
  void failAlways(Op op, std::string code) {
    std::lock_guard<std::mutex> lock(mutex_);
    always_[op] = std::move(code);
  }
  /// The next `op` waits until the gate is opened.
  std::shared_ptr<Gate> holdNext(Op op) {
    auto gate = std::make_shared<Gate>();
    std::lock_guard<std::mutex> lock(mutex_);
    holds_[op].push_back(gate);
    return gate;
  }
  /// Something another process does just before every `op`.
  void before(Op op, std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    before_[op] = std::move(hook);
  }

  static std::string name(Op op) {
    switch (op) {
      case Op::ReadFile: return "readFile";
      case Op::Create: return "create";
      case Op::Write: return "write";
      case Op::Sync: return "sync";
      case Op::Close: return "close";
      case Op::Rename: return "rename";
      case Op::Rm: return "rm";
      case Op::Readdir: return "readdir";
      case Op::MtimeMs: return "mtimeMs";
      case Op::Mkdir: return "mkdir";
      case Op::SyncDir: return "syncDir";
    }
    return "?";
  }

 private:
  class File final : public keygrant::StorageFile {
   public:
    File(MemoryFs& fs, std::string path) : fs_(fs), path_(std::move(path)) {}
    void write(const std::string& data) override {
      fs_.enter(Op::Write, path_, true);
      std::lock_guard<std::mutex> lock(fs_.mutex_);
      fs_.files_[path_] = data;
    }
    void sync() override { fs_.enter(Op::Sync, path_, false); }
    void close() override { fs_.enter(Op::Close, path_, false); }

   private:
    MemoryFs& fs_;
    std::string path_;
  };

  void enter(Op op, const std::string& path, bool changing) {
    std::function<void()> hook;
    std::shared_ptr<Gate> hold;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = before_.find(op); it != before_.end()) hook = it->second;
      if (auto& queue = holds_[op]; !queue.empty()) {
        hold = queue.front();
        queue.pop_front();
      }
    }
    if (hook) hook();
    if (hold) hold->wait();
    std::lock_guard<std::mutex> lock(mutex_);
    std::string code;
    if (auto& queue = queued_[op]; !queue.empty()) {
      code = queue.front();
      queue.pop_front();
    } else if (const auto it = always_.find(op); it != always_.end()) {
      code = it->second;
    }
    if (!code.empty()) throw keygrant::IoError(code, name(op) + " " + path);
    ops_.push_back(name(op) + " " + path);
    if (changing) changes_.push_back(name(op) + " " + path);
  }

  std::mutex mutex_;
  std::map<std::string, std::string> files_;
  std::map<std::string, std::int64_t> mtimes_;
  std::vector<std::string> ops_;
  std::vector<std::string> changes_;
  std::map<Op, std::deque<std::string>> queued_;
  std::map<Op, std::string> always_;
  std::map<Op, std::deque<std::shared_ptr<Gate>>> holds_;
  std::map<Op, std::function<void()>> before_;
};

/** Waits that take no time, recorded: what a retry would have slept. */
struct NoSleep {
  std::shared_ptr<std::vector<Millis>> slept = std::make_shared<std::vector<Millis>>();
  std::function<void(Millis)> fn() const {
    auto record = slept;
    return [record](Millis ms) { record->push_back(ms); };
  }
  std::int64_t total() const {
    std::int64_t sum = 0;
    for (const Millis ms : *slept) sum += ms.count();
    return sum;
  }
};

/** Where the in-memory state lives. */
inline std::string memoryDir() {
#ifdef _WIN32
  return "C:\\keygrant-memory";
#else
  return "/keygrant-memory";
#endif
}
inline std::string memoryState() { return keygrant::detail::joinPath(memoryDir(), "sluice.json"); }

/** A FileStorage over a memory fs, waits recorded. */
inline std::shared_ptr<keygrant::FileStorage> storageOver(const std::shared_ptr<MemoryFs>& fs, const NoSleep& sleep = {},
                                                          std::optional<std::string> migrateFrom = std::nullopt) {
  keygrant::FileStorageOptions options;
  options.fs = fs;
  options.sleep = sleep.fn();
  options.migrateFrom = std::move(migrateFrom);
  return std::make_shared<keygrant::FileStorage>(memoryState(), options);
}

inline const std::string kSaved = R"({"key":"SLUICE-AAAA-BBBB-CCCC-DDDD","activationId":"act_1","lastSeen":1})";

}  // namespace kgtest
