#include "machine_id.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "bounded.hpp"
#include "utf.hpp"

namespace keygrant {

namespace detail {

namespace {

/// The latest boot time read as one (JavaScript's safe integers, in seconds).
constexpr std::int64_t kMaxSafeSeconds = 9'007'199'254'740;

/** One line of mountinfo that holds /etc/machine-id: its id, its parent's, its mount point and type. */
struct Mount {
  std::string id;
  std::string parent;
  std::string point;
  std::string fstype;
};

std::vector<std::string> split(const std::string& text, char by) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (;;) {
    const std::size_t end = text.find(by, start);
    parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) return parts;
    start = end + 1;
  }
}

/** A mount point with its octal escapes (`\040` for a space) undone. */
std::string unescaped(const std::string& raw) {
  std::string out;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const auto octal = [&](std::size_t at) { return at < raw.size() && raw[at] >= '0' && raw[at] <= '7'; };
    if (raw[i] == '\\' && octal(i + 1) && octal(i + 2) && octal(i + 3)) {
      const int value = (raw[i + 1] - '0') * 64 + (raw[i + 2] - '0') * 8 + (raw[i + 3] - '0');
      // Above a byte is a character no path compared here holds: kept as one that never matches.
      out.push_back(value <= 0xFF ? static_cast<char>(value) : '\x01');
      i += 3;
    } else {
      out.push_back(raw[i]);
    }
  }
  return out;
}

/** Of mounts at one point, the one on top: the one no other there names as its parent; else the last listed. */
const Mount* topMount(const std::vector<const Mount*>& atPoint) {
  std::vector<const Mount*> tops;
  for (const Mount* m : atPoint) {
    const bool covered = std::any_of(atPoint.begin(), atPoint.end(),
                                     [&](const Mount* other) { return other != m && other->parent == m->id; });
    if (!covered) tops.push_back(m);
  }
  if (tops.size() == 1) return tops.front();
  return atPoint.empty() ? nullptr : atPoint.back();
}

/** The first run of non-space characters, after any leading space (a regex's \s+(\S+)). */
std::string firstToken(const std::string& text) {
  const std::vector<CodePoint> points = decodeUtf8(text);
  std::size_t i = 0;
  while (i < points.size() && isJsSpace(points[i].value)) ++i;
  std::size_t end = i;
  while (end < points.size() && !isJsSpace(points[end].value)) ++end;
  if (i == end) return {};
  return text.substr(points[i].offset, points[end - 1].offset + points[end - 1].length - points[i].offset);
}

MachineIdReading idOrUnavailable(const std::string& id) {
  return id.empty() ? MachineIdReading::unavailable() : MachineIdReading::of(id);
}

/** Filesystems held in memory: an id on one of them is written afresh every boot. */
bool inMemory(const std::optional<std::string>& fstype) { return fstype == "tmpfs" || fstype == "ramfs"; }

/** Whether /etc/machine-id lives on a filesystem in memory. False when mountinfo cannot be read. */
bool transientMachineId(MachineIdSource& source) {
  try {
    return inMemory(machineIdFilesystem(source.readFile("/proc/self/mountinfo")));
  } catch (...) {
    return false;
  }
}

/** A file's id, or why there is none. */
MachineIdReading fromFile(MachineIdSource& source, const std::string& path) {
  try {
    return idOrUnavailable(jsTrim(source.readFile(path)));
  } catch (...) {
    return failureOf(std::current_exception());
  }
}

/**
 * Whether the id file was written at this boot: its content (mtime, never
 * ctime) no older than the boot time less the slack. False when either cannot
 * be read, which leaves the id read as it always was.
 */
bool writtenThisBoot(MachineIdSource& source, const std::string& path) {
  try {
    const std::optional<std::int64_t> written = source.mtimeMs(path);
    if (!written) return false;
    const std::optional<std::int64_t> boot = bootTimeMs(source.readFile("/proc/stat"));
    return boot && *written >= *boot - kBootSlackMs;
  } catch (...) {
    return false;
  }
}

}  // namespace

MachineIdReading machineGuidFrom(const RegistryRead& read) {
  switch (read.status) {
    case kWinSuccess: {
      const std::u16string data = read.data.substr(0, read.data.find(u'\0'));
      return idOrUnavailable(firstToken(utf16ToUtf8(data)));
    }
    case kWinFileNotFound:
    case kWinPathNotFound:
    case kWinAccessDenied:
    case kWinUnsupportedType:
      return MachineIdReading::unavailable();
    default:
      return MachineIdReading::transient();
  }
}

MachineIdReading platformUuidFrom(const IoKitRead& read) {
  switch (read.status) {
    case IoKitRead::Status::Ok:
      return idOrUnavailable(jsTrim(read.value.substr(0, read.value.find('"'))));
    case IoKitRead::Status::NoService:
    case IoKitRead::Status::NoProperty:
    case IoKitRead::Status::NotAString:
      return MachineIdReading::unavailable();
    case IoKitRead::Status::Failed:
      return MachineIdReading::transient();
  }
  return MachineIdReading::transient();
}

std::optional<std::string> machineIdFilesystem(const std::string& mountinfo) {
  const std::string target = kMachineIdFile;
  std::vector<Mount> mounts;
  for (const std::string& line : split(mountinfo, '\n')) {
    const std::vector<std::string> fields = split(line, ' ');
    if (fields.size() < 5) continue;
    std::size_t separator = 0;
    for (std::size_t i = 6; i < fields.size(); ++i) {
      if (fields[i] == "-") {
        separator = i;
        break;
      }
    }
    if (separator == 0 || separator + 1 >= fields.size()) continue;
    const std::string point = unescaped(fields[4]);
    if (point == target || point == "/" || target.rfind(point + "/", 0) == 0) {
      mounts.push_back(Mount{fields[0], fields[1], point, fields[separator + 1]});
    }
  }
  std::size_t deepest = 0;
  for (const Mount& m : mounts) deepest = std::max(deepest, m.point.size());
  std::vector<const Mount*> atPoint;
  for (const Mount& m : mounts) {
    if (m.point.size() == deepest) atPoint.push_back(&m);
  }
  const Mount* top = topMount(atPoint);
  if (top == nullptr) return std::nullopt;
  return top->fstype;
}

std::string hostNameFrom(std::string_view bytes) { return sanitizeUtf8(bytes); }

std::optional<std::int64_t> bootTimeMs(const std::string& procStat) {
  std::string line;
  for (std::size_t i = 0; i <= procStat.size(); ++i) {
    if (i < procStat.size() && procStat[i] != '\n' && procStat[i] != '\r') {
      line.push_back(procStat[i]);
      continue;
    }
    if (line.size() > 6 && line.rfind("btime ", 0) == 0 &&
        std::all_of(line.begin() + 6, line.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      std::int64_t seconds = 0;
      for (std::size_t d = 6; d < line.size(); ++d) {
        // Beyond any real time: saturated, so no file reads as newer than it.
        if (seconds > kMaxSafeSeconds) return kMaxSafeSeconds * 1000;
        seconds = seconds * 10 + (line[d] - '0');
      }
      return std::min(seconds, kMaxSafeSeconds) * 1000;
    }
    line.clear();
  }
  return std::nullopt;
}

MachineIdReading failureOf(std::exception_ptr error) {
  try {
    if (error) std::rethrow_exception(error);
  } catch (const IoError& failure) {
    const std::string& code = failure.code();
    if (code == "ENOENT" || code == "EACCES" || code == "EPERM" || code == "ENOTDIR" || code == "EISDIR") {
      return MachineIdReading::unavailable();
    }
  } catch (...) {
    // Anything unexpected is this time only: guessing "for good" is what flips a fingerprint.
  }
  return MachineIdReading::transient();
}

MachineIdReading readMachineIdNow(MachineIdSource& source) {
  try {
    const std::string platform = source.platform();
    if (platform == "win32") return machineGuidFrom(source.readMachineGuid());
    if (platform == "darwin") return platformUuidFrom(source.readPlatformUuid());
    // An id on a filesystem in memory is new every boot: no id to hold a device to.
    if (transientMachineId(source)) return MachineIdReading::unavailable();
    for (const char* path : {kMachineIdFile, kDbusMachineIdFile}) {
      const MachineIdReading reading = fromFile(source, path);
      if (reading.kind == MachineIdReading::Kind::Unavailable) continue;
      // Written at this boot: a volatile root, whatever its filesystem.
      if (reading.kind == MachineIdReading::Kind::Id && writtenThisBoot(source, path)) {
        return MachineIdReading::unavailable();
      }
      return reading;
    }
    return MachineIdReading::unavailable();
  } catch (...) {
    return failureOf(std::current_exception());
  }
}

namespace {

using Read = SharedCall<MachineIdReading>;

/**
 * The read of this source's id in flight, else a new one: one at a time per
 * source, so a read that never ends (a hung root filesystem, a filter driver)
 * costs one thread, not one more on every call. A source with a read in flight
 * is held by it, so its address names no other while the read runs.
 */
std::shared_ptr<Read> readOf(const std::shared_ptr<MachineIdSource>& source) {
  // Never destroyed: an abandoned read may still finish at exit.
  static auto* mutex = new std::mutex();
  static auto* inFlight = new std::map<const MachineIdSource*, std::shared_ptr<Read>>();
  std::lock_guard<std::mutex> lock(*mutex);
  for (auto it = inFlight->begin(); it != inFlight->end();) {
    it = it->second->finished() ? inFlight->erase(it) : std::next(it);
  }
  std::shared_ptr<Read>& read = (*inFlight)[source.get()];
  if (!read) {
    const std::shared_ptr<MachineIdSource> held = source;
    read = Read::start([held] { return readMachineIdNow(*held); });
  }
  return read;
}

}  // namespace

}  // namespace detail

MachineIdReading readMachineId(const std::shared_ptr<MachineIdSource>& source, std::chrono::milliseconds timeout) {
  if (!source) return MachineIdReading::transient();
  // Waited for until shortly after its time box, even when the read itself does
  // not end; a read already in flight for this source is waited for, not doubled.
  const auto reading = detail::readOf(source)->wait(timeout + std::chrono::milliseconds{500});
  return reading.value_or(MachineIdReading::transient());
}

}  // namespace keygrant
