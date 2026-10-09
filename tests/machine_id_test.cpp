// The machine id and the default fingerprinter: the parsing and decisions are
// portable and tested here on every platform; the reads behind them are the
// registry API on Windows and IOKit on macOS.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

#include "machine_id.hpp"
#include "support.hpp"
#include "sys.hpp"
#include "utf.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/utsname.h>
#endif
#ifdef __linux__
#include <sys/statfs.h>
#include <sys/sysinfo.h>
#endif

namespace kgtest {
namespace {

using namespace keygrant::detail;
using keygrant::IoKitRead;
using keygrant::IoError;
using keygrant::MachineIdReading;
using keygrant::RegistryRead;
using namespace std::chrono_literals;

const std::string GUID = "3f2504e0-4f89-11d3-9a0c-0305e82c3301";

std::u16string wide(const std::string& ascii) { return std::u16string(ascii.begin(), ascii.end()); }

TEST(RegistryValue, IsTheMachineGuidAsRegQueryPrintsIt) {
  // As RegGetValueW returns REG_SZ: with its terminating NUL, or without one.
  EXPECT_EQ(machineGuidFrom({kWinSuccess, wide(GUID) + u'\0'}), MachineIdReading::of(GUID));
  EXPECT_EQ(machineGuidFrom({kWinSuccess, wide(GUID)}), MachineIdReading::of(GUID));
  EXPECT_EQ(machineGuidFrom({kWinSuccess, wide(GUID) + u'\0' + u'\0'}), MachineIdReading::of(GUID));
  // Case kept: a MachineGuid is read as written.
  EXPECT_EQ(machineGuidFrom({kWinSuccess, wide("3F2504E0-4F89")}), MachineIdReading::of("3F2504E0-4F89"));
  // The first run of non-space characters, as `REG_SZ\s+(\S+)` takes it.
  EXPECT_EQ(machineGuidFrom({kWinSuccess, wide("  \t" + GUID + "  ")}), MachineIdReading::of(GUID));
  EXPECT_EQ(machineGuidFrom({kWinSuccess, wide("abc def")}), MachineIdReading::of("abc"));
  // Nothing in it is no id.
  EXPECT_EQ(machineGuidFrom({kWinSuccess, u""}), MachineIdReading::unavailable());
  EXPECT_EQ(machineGuidFrom({kWinSuccess, wide("   ") + u'\0'}), MachineIdReading::unavailable());
}

TEST(RegistryValue, IsUnavailableWhenNotThereOrNotOursAndTransientOtherwise) {
  for (const long status : {kWinFileNotFound, kWinPathNotFound, kWinAccessDenied, kWinUnsupportedType}) {
    EXPECT_EQ(machineGuidFrom({status, {}}), MachineIdReading::unavailable()) << status;
  }
  for (const long status : {kWinMoreData, 31L, 87L, 1450L}) {
    EXPECT_EQ(machineGuidFrom({status, {}}), MachineIdReading::transient()) << status;
  }
}

/** What the reference takes out of reg.exe's line for a value whose data is `data` (`idIn`: the match, trimmed). */
MachineIdReading fromRegQuery(const std::string& data) {
  const std::string output =
      "\r\nHKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Cryptography\r\n    MachineGuid    REG_SZ    " + data + "\r\n\r\n";
  std::smatch match;
  if (!std::regex_search(output, match, std::regex(R"(MachineGuid\s+REG_SZ\s+(\S+))"))) {
    return MachineIdReading::unavailable();
  }
  const std::string id = jsTrim(match[1].str());
  return id.empty() ? MachineIdReading::unavailable() : MachineIdReading::of(id);
}

TEST(RegistryValue, GivesTheIdTheReferenceTakesOutOfRegQueryOutput) {
  // reg.exe prints a value's data as it is; the reference takes `REG_SZ\s+(\S+)`
  // out of that line. The data where the two readings could part: white space
  // before, inside or after it, a line break inside it, nothing but white space.
  for (const std::string& data :
       std::vector<std::string>{GUID, "  " + GUID, "\t" + GUID, GUID + "  ", GUID + "\t", "abc def", " \t abc\tdef ",
                                "abc\r\ndef", "{" + GUID + "}", "", "   ", "\t \t"}) {
    EXPECT_EQ(machineGuidFrom({kWinSuccess, wide(data) + u'\0'}), fromRegQuery(data)) << "[" << data << "]";
  }
}

TEST(IoKitValue, IsThePlatformUuidAsIoregPrintsIt) {
  const std::string uuid = "564D8A1C-3F6E-4E3B-9F2A-1B2C3D4E5F60";
  EXPECT_EQ(platformUuidFrom({IoKitRead::Status::Ok, uuid}), MachineIdReading::of(uuid));
  EXPECT_EQ(platformUuidFrom({IoKitRead::Status::Ok, " " + uuid + "\n"}), MachineIdReading::of(uuid));
  EXPECT_EQ(platformUuidFrom({IoKitRead::Status::Ok, ""}), MachineIdReading::unavailable());
  EXPECT_EQ(platformUuidFrom({IoKitRead::Status::Ok, "   "}), MachineIdReading::unavailable());
  EXPECT_EQ(platformUuidFrom({IoKitRead::Status::NoService, ""}), MachineIdReading::unavailable());
  EXPECT_EQ(platformUuidFrom({IoKitRead::Status::NoProperty, ""}), MachineIdReading::unavailable());
  EXPECT_EQ(platformUuidFrom({IoKitRead::Status::NotAString, ""}), MachineIdReading::unavailable());
  EXPECT_EQ(platformUuidFrom({IoKitRead::Status::Failed, ""}), MachineIdReading::transient());
}

/** What the reference takes out of `ioreg -rd1 -c IOPlatformExpertDevice` for a UUID of `value` (`idIn`: the match, trimmed). */
MachineIdReading fromIoreg(const std::string& value) {
  const std::string output = "+-o J314sAP  <class IOPlatformExpertDevice, id 0x100000110, registered>\n    {\n"
                             "      \"IOPlatformSerialNumber\" = \"C02XXXXXXX\"\n      \"IOPlatformUUID\" = \"" +
                             value + "\"\n    }\n";
  std::smatch match;
  if (!std::regex_search(output, match, std::regex(R"re("IOPlatformUUID"\s*=\s*"([^"]+)")re"))) {
    return MachineIdReading::unavailable();
  }
  const std::string id = jsTrim(match[1].str());
  return id.empty() ? MachineIdReading::unavailable() : MachineIdReading::of(id);
}

TEST(IoKitValue, GivesTheIdTheReferenceTakesOutOfIoregOutput) {
  // ioreg prints the string as it is, in quotes; the reference takes
  // `"IOPlatformUUID"\s*=\s*"([^"]+)"` out of that and trims it. The values
  // where the two readings could part: white space around or inside it, a
  // quote inside it, nothing at all.
  const std::string uuid = "564D8A1C-3F6E-4E3B-9F2A-1B2C3D4E5F60";
  for (const std::string& value : std::vector<std::string>{uuid, " " + uuid + " ", "\t" + uuid, uuid + "\n", "AB CD",
                                                           "AB\"CD", "\"" + uuid, "\"", "", "   "}) {
    EXPECT_EQ(platformUuidFrom({IoKitRead::Status::Ok, value}), fromIoreg(value)) << "[" << value << "]";
  }
}

const std::string ROOT = "22 1 8:1 / / rw,relatime shared:1 - ext4 /dev/sda1 rw";
const std::string OVER_MACHINE_ID = "29 23 0:25 /machine-id /etc/machine-id ro,relatime shared:5 - tmpfs tmpfs rw";
const std::string ROOT_TMPFS = "1 0 0:1 / / rw,relatime shared:1 - tmpfs tmpfs rw";

std::string lines(std::initializer_list<std::string> all) {
  std::string out;
  for (const std::string& line : all) out += line + "\n";
  return out;
}

TEST(Mountinfo, ClassifiesByTheFilesystemHoldingTheId) {
  struct Case {
    const char* name;
    std::string mountinfo;
    std::optional<std::string> fstype;
  };
  const std::vector<Case> cases = {
      {"nothing mounted at /etc/machine-id", lines({ROOT}), "ext4"},
      {"systemd's transient id: a /run tmpfs file bound over it", lines({ROOT, OVER_MACHINE_ID}), "tmpfs"},
      {"the path as a mount's root, not its mount point",
       lines({ROOT, "30 23 0:25 /etc/machine-id /run/host-id ro,relatime - tmpfs tmpfs rw"}), "ext4"},
      {"a mount point it is only a prefix of", lines({ROOT, "31 23 0:26 / /etc/machine-id.d rw,relatime - tmpfs tmpfs rw"}), "ext4"},
      {"an impermanent root", lines({ROOT_TMPFS}), "tmpfs"},
      {"an impermanent /etc", lines({ROOT, "2 1 0:2 / /etc rw,relatime shared:2 - tmpfs none rw"}), "tmpfs"},
      {"no optional field", lines({ROOT, "2 1 0:2 / /etc rw - tmpfs none rw"}), "tmpfs"},
      {"two optional fields", lines({ROOT, "2 1 0:2 / /etc rw shared:2 master:1 - tmpfs tmpfs rw"}), "tmpfs"},
      {"an overmount listed BEFORE the mount it covers",
       lines({"5 29 8:1 /etc/machine-id /etc/machine-id ro - ext4 /dev/sda1 rw", ROOT, OVER_MACHINE_ID}), "ext4"},
      {"a prefix of the path but not a parent of it", lines({ROOT, "2 1 0:2 / /et rw - tmpfs tmpfs rw"}), "ext4"},
      {"an escaped mount point (\\143 is c)", lines({ROOT, "2 1 0:2 / /et\\143 rw - tmpfs tmpfs rw"}), "tmpfs"},
      {"a ramfs /etc", lines({ROOT, "2 1 0:2 / /etc rw master:4 - ramfs ramfs rw"}), "ramfs"},
      {"a stable id bound in from a disk", lines({ROOT, "3 1 8:1 /etc/machine-id /etc/machine-id ro,relatime - ext4 /dev/sda1 rw"}), "ext4"},
      {"a persisted id over an impermanent root",
       lines({ROOT_TMPFS, "3 1 8:2 /persist/etc/machine-id /etc/machine-id rw - btrfs /dev/sda2 rw"}), "btrfs"},
      {"a disk bound over a tmpfs one at the same point",
       lines({ROOT, OVER_MACHINE_ID, "5 29 8:1 /etc/machine-id /etc/machine-id ro - ext4 /dev/sda1 rw"}), "ext4"},
      {"two at one point, neither over the other, a tmpfs last",
       lines({ROOT, "3 1 8:1 /etc/machine-id /etc/machine-id ro - ext4 /dev/sda1 rw",
              "4 1 0:25 /machine-id /etc/machine-id ro - tmpfs tmpfs rw"}),
       "tmpfs"},
      {"two at one point, neither over the other, a disk last",
       lines({ROOT, "4 1 0:25 /machine-id /etc/machine-id ro - tmpfs tmpfs rw",
              "3 1 8:1 /etc/machine-id /etc/machine-id ro - ext4 /dev/sda1 rw"}),
       "ext4"},
      {"a separator with no type after it", "22 1 8:1 / / rw -", std::nullopt},
      {"empty", "", std::nullopt},
  };
  for (const Case& c : cases) EXPECT_EQ(machineIdFilesystem(c.mountinfo), c.fstype) << c.name;
}

TEST(BootTime, IsTheBtimeLineInMilliseconds) {
  EXPECT_EQ(bootTimeMs("cpu 1 2 3\nbtime 1757000000\nprocesses 9\n"), 1'757'000'000'000);
  EXPECT_EQ(bootTimeMs("btime 1757000000\r\n"), 1'757'000'000'000);
  EXPECT_EQ(bootTimeMs("cpu 1 2 3\n"), std::nullopt);
  EXPECT_EQ(bootTimeMs("btime \n"), std::nullopt);
  EXPECT_EQ(bootTimeMs("btime 12x\n"), std::nullopt);
  EXPECT_EQ(bootTimeMs(" btime 12\n"), std::nullopt);
  // Absurdly large: saturated, so no file reads as written after it.
  EXPECT_GT(*bootTimeMs("btime 99999999999999999999999\n"), 9'000'000'000'000'000);
}

/** A stand-in system: files by path (content, or an errno code), and their mtimes. */
class FakeSource final : public keygrant::MachineIdSource {
 public:
  explicit FakeSource(std::string platform = "linux") : platform_(std::move(platform)) {}
  std::string platform() override { return platform_; }
  RegistryRead readMachineGuid() override {
    ++reads;
    if (hang) {
      ++held;
      gate.wait();
      --held;
    }
    if (registry) return *registry;
    return {kWinFileNotFound, {}};
  }
  IoKitRead readPlatformUuid() override {
    ++reads;
    if (iokitThrows) throw std::runtime_error("spawn failed");
    return iokit;
  }
  std::string readFile(const std::string& path) override {
    if (hang) gate.wait();
    const auto it = files.find(path);
    if (it == files.end()) throw IoError("ENOENT", path);
    if (it->second.rfind("!E", 0) == 0) throw IoError(it->second.substr(1), path);
    return it->second;
  }
  std::optional<std::int64_t> mtimeMs(const std::string& path) override {
    const auto it = mtimes.find(path);
    if (it == mtimes.end()) return std::nullopt;
    if (it->second < 0) throw IoError("EACCES", path);
    return it->second;
  }

  std::map<std::string, std::string> files;
  std::map<std::string, std::int64_t> mtimes;
  std::optional<RegistryRead> registry;
  IoKitRead iokit{IoKitRead::Status::NoService, {}};
  bool iokitThrows = false;
  std::atomic<bool> hang{false};
  std::atomic<int> reads{0};
  std::atomic<int> held{0};
  Gate gate;

 private:
  std::string platform_;
};

TEST(MachineIdLinux, ReadsTheSystemdFileElseTheDbusOneTrimmed) {
  const auto id = [](std::map<std::string, std::string> files) {
    FakeSource source;
    source.files = std::move(files);
    return readMachineIdNow(source);
  };
  EXPECT_EQ(id({{"/etc/machine-id", "a1b2c3d4e5f6\n"}}), MachineIdReading::of("a1b2c3d4e5f6"));
  EXPECT_EQ(id({{"/var/lib/dbus/machine-id", "dbus0001\n"}}), MachineIdReading::of("dbus0001"));
  EXPECT_EQ(id({{"/etc/machine-id", "\n"}, {"/var/lib/dbus/machine-id", "dbus0002"}}), MachineIdReading::of("dbus0002"));
  EXPECT_EQ(id({{"/etc/machine-id", "!EACCES"}, {"/var/lib/dbus/machine-id", "dbus0003"}}), MachineIdReading::of("dbus0003"));
  EXPECT_EQ(id({}), MachineIdReading::unavailable());
  EXPECT_EQ(id({{"/etc/machine-id", "!EIO"}, {"/var/lib/dbus/machine-id", "dbus0004"}}), MachineIdReading::transient());
}

TEST(MachineIdLinux, HasNoIdOnAFilesystemInMemoryAndReadsItWhenMountinfoCannotBeRead) {
  FakeSource source;
  source.files = {{"/proc/self/mountinfo", lines({ROOT, OVER_MACHINE_ID})}, {"/etc/machine-id", "transient-id\n"}};
  EXPECT_EQ(readMachineIdNow(source), MachineIdReading::unavailable());
  for (const char* unreadable : {"!EIO", "!EACCES"}) {
    source.files["/proc/self/mountinfo"] = unreadable;
    EXPECT_EQ(readMachineIdNow(source), MachineIdReading::of("transient-id"));
  }
}

TEST(MachineIdLinux, HasNoIdWrittenAtThisBoot) {
  const std::int64_t btime = 1'757'000'000;
  const std::int64_t booted = btime * 1000;
  const auto booted_with = [&](std::optional<std::int64_t> mtime, std::string procStat) {
    FakeSource source;
    source.files = {{"/proc/self/mountinfo", lines({ROOT})}, {"/etc/machine-id", "a1b2c3d4e5f6\n"}, {"/proc/stat", procStat}};
    if (mtime) source.mtimes["/etc/machine-id"] = *mtime;
    return readMachineIdNow(source);
  };
  const std::string procStat = "cpu 1 2 3\nbtime " + std::to_string(btime) + "\n";
  const MachineIdReading id = MachineIdReading::of("a1b2c3d4e5f6");
  EXPECT_EQ(booted_with(booted + 3'000, procStat), MachineIdReading::unavailable());
  EXPECT_EQ(booted_with(booted - 90 * DAY, procStat), id);
  EXPECT_EQ(booted_with(booted - keygrant::kBootSlackMs, procStat), MachineIdReading::unavailable());
  EXPECT_EQ(booted_with(booted - keygrant::kBootSlackMs - 1, procStat), id);
  // When /proc/stat or the file's time cannot be read, the id as always.
  EXPECT_EQ(booted_with(booted + 3'000, "cpu 1 2 3\n"), id);
  EXPECT_EQ(booted_with(-1, procStat), id);
  EXPECT_EQ(booted_with(std::nullopt, procStat), id);
}

TEST(MachineIdFailures, AreForGoodWhenNothingIsThereAndThisTimeOnlyOtherwise) {
  for (const char* code : {"ENOENT", "EACCES", "EPERM", "ENOTDIR", "EISDIR"}) {
    FakeSource source;
    source.files["/etc/machine-id"] = std::string("!") + code;
    source.files["/var/lib/dbus/machine-id"] = std::string("!") + code;
    EXPECT_EQ(readMachineIdNow(source), MachineIdReading::unavailable()) << code;
  }
  for (const char* code : {"EAGAIN", "EIO", "EMFILE"}) {
    FakeSource source;
    source.files["/etc/machine-id"] = std::string("!") + code;
    EXPECT_EQ(readMachineIdNow(source), MachineIdReading::transient()) << code;
  }
  FakeSource mac("darwin");
  mac.iokitThrows = true;
  EXPECT_EQ(readMachineIdNow(mac), MachineIdReading::transient());
  EXPECT_EQ(failureOf(nullptr), MachineIdReading::transient());
}

TEST(MachineIdOtherPlatforms, ReadTheirOwnSources) {
  FakeSource windows("win32");
  windows.registry = RegistryRead{kWinSuccess, wide(GUID) + u'\0'};
  EXPECT_EQ(readMachineIdNow(windows), MachineIdReading::of(GUID));
  FakeSource mac("darwin");
  mac.iokit = IoKitRead{IoKitRead::Status::Ok, "UUID-1"};
  EXPECT_EQ(readMachineIdNow(mac), MachineIdReading::of("UUID-1"));
  // Anything else reads the Linux files: FreeBSD has neither.
  EXPECT_EQ(readMachineIdNow(*std::make_shared<FakeSource>("freebsd")), MachineIdReading::unavailable());
}

TEST(MachineIdBound, AReadThatNeverFinishesIsTransientOnceItsTimeIsOut) {
  auto source = std::make_shared<FakeSource>("win32");
  source->hang = true;
  MachineIdReading reading;
  const Millis took = timed([&] { reading = keygrant::readMachineId(source, Millis{50}); });
  EXPECT_EQ(reading, MachineIdReading::transient());
  EXPECT_GE(took, Millis{500});
  EXPECT_LT(took, Millis{3'000});
  source->gate.open();
  for (int i = 0; i < 500 && source->held.load() > 0; ++i) std::this_thread::sleep_for(Millis{10});
}

TEST(MachineIdBound, AReadThatNeverEndsIsMadeOnceHoweverManyCallsWaitOnIt) {
  // A hung read costs one thread, not one more on every call.
  auto source = std::make_shared<FakeSource>("win32");
  source->hang = true;
  for (int i = 0; i < 3; ++i) EXPECT_EQ(keygrant::readMachineId(source, Millis{0}), MachineIdReading::transient());
  EXPECT_EQ(source->reads.load(), 1);
  // Once it ends, the next call reads afresh. (The id is there before the hung
  // read is let go, so whichever read a call gets, it gets the id.)
  source->hang = false;
  source->registry = RegistryRead{kWinSuccess, wide(GUID)};
  source->gate.open();
  for (int i = 0; i < 100 && source->reads.load() < 2; ++i) {
    EXPECT_EQ(keygrant::readMachineId(source, Millis{200}), MachineIdReading::of(GUID));
    if (source->reads.load() < 2) std::this_thread::sleep_for(Millis{10});
  }
  EXPECT_EQ(source->reads.load(), 2);
}

const std::string& fallback() {
  static const std::string legacy = keygrant::hostnameFingerprint();
  return legacy;
}

TEST(SystemFingerprinter, HashesTheMachineIdAndOffersTheHostNameAsAnAlternate) {
  auto source = std::make_shared<FakeSource>("win32");
  source->registry = RegistryRead{kWinSuccess, wide(GUID)};
  keygrant::SystemFingerprinter fingerprinter(source);
  const auto identity = fingerprinter.identify(Millis{200});
  ASSERT_TRUE(identity);
  EXPECT_EQ(*identity, (keygrant::DeviceIdentity{keygrant::machineFingerprint(GUID), DeviceKind::Machine, {fallback()}}));
  EXPECT_EQ(fingerprinter.get(), keygrant::hashFingerprint("machine-id|" + GUID));
  EXPECT_EQ(fingerprinter.get().find("3f2504e0"), std::string::npos);
  // Settled: read once for the fingerprinter.
  EXPECT_EQ(source->reads, 1);
}

TEST(SystemFingerprinter, FallsBackToTheHostNameForGoodWhenThereIsNoIdToRead) {
  for (const char* platform : {"win32", "darwin", "linux", "freebsd"}) {
    auto source = std::make_shared<FakeSource>(platform);
    keygrant::SystemFingerprinter fingerprinter(source);
    EXPECT_EQ(fingerprinter.get(), fallback()) << platform;
    EXPECT_EQ(fingerprinter.identify(Millis{200}), (keygrant::DeviceIdentity{fallback(), DeviceKind::Hostname, {}}))
        << platform;
  }
}

TEST(SystemFingerprinter, GivesNoFingerprintForAReadThatDidNotFinishAndReadsAgainNextTime) {
  auto source = std::make_shared<FakeSource>();
  source->files["/etc/machine-id"] = "!EIO";
  keygrant::SystemFingerprinter fingerprinter(source);
  EXPECT_EQ(fingerprinter.identify(Millis{200}), (keygrant::DeviceIdentity{std::nullopt, std::nullopt, {fallback()}}));
  EXPECT_THROW(fingerprinter.get(), std::runtime_error);
  // Read again, and kept once it has the id.
  source->files["/etc/machine-id"] = "a1b2c3d4e5f6\n";
  EXPECT_EQ(fingerprinter.identify(Millis{200})->fingerprint, keygrant::machineFingerprint("a1b2c3d4e5f6"));
  source->files["/etc/machine-id"] = "!EIO";
  EXPECT_EQ(fingerprinter.identify(Millis{200})->fingerprint, keygrant::machineFingerprint("a1b2c3d4e5f6"));
}

TEST(SystemFingerprinter, SurvivesARenameTheFingerprintFollowsTheMachineId) {
  auto before = std::make_shared<FakeSource>();
  before->files["/etc/machine-id"] = "same-machine";
  auto after = std::make_shared<FakeSource>();
  after->files["/etc/machine-id"] = "same-machine";
  EXPECT_EQ(keygrant::SystemFingerprinter(before).get(), keygrant::SystemFingerprinter(after).get());
  EXPECT_NE(keygrant::SystemFingerprinter(before).get(), fallback());
}

/** This machine's host name, from a source the SDK does not read it from. */
std::optional<std::string> hostNameElsewhere() {
#ifdef _WIN32
  wchar_t buffer[256] = {};
  DWORD size = 256;
  if (!GetComputerNameExW(ComputerNameDnsHostname, buffer, &size)) return std::nullopt;
  return keygrant::detail::narrow(buffer);
#else
  struct utsname names {};
  if (uname(&names) != 0) return std::nullopt;
  return std::string(names.nodename);
#endif
}

TEST(Fingerprints, AreNodesNamesForThisMachine) {
  EXPECT_TRUE(std::regex_match(keygrant::hashFingerprint("abc"), std::regex("^[0-9a-f]{64}$")));
  // The host name as the system says it elsewhere, and Node's names for this build.
  const auto elsewhere = hostNameElsewhere();
  ASSERT_TRUE(elsewhere);
#if defined(_WIN32)
  const char* platform = "win32";
#elif defined(__APPLE__)
  const char* platform = "darwin";
#elif defined(__linux__)
  const char* platform = "linux";
#else
  const char* platform = nullptr;
#endif
#if defined(__x86_64__) || defined(_M_X64)
  const char* arch = "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  const char* arch = "arm64";
#else
  const char* arch = nullptr;
#endif
  if (platform != nullptr) {
    EXPECT_EQ(keygrant::platformName(), platform);
  }
  if (arch != nullptr) {
    EXPECT_EQ(keygrant::archName(), arch);
  }
  EXPECT_EQ(keygrant::hostName(), *elsewhere);
  EXPECT_EQ(fallback(), keygrant::hashFingerprint(*elsewhere + "|" + keygrant::platformName() + "|" + keygrant::archName()));
#ifdef __linux__
  // And as the kernel holds it.
  std::ifstream file("/proc/sys/kernel/hostname");
  std::string kernel;
  if (std::getline(file, kernel)) {
    EXPECT_EQ(keygrant::hostName(), kernel);
  }
#endif
}

TEST(HostName, IsReadAsNodeReadsTheBytesTheSystemHolds) {
  // Each ill-formed sequence one U+FFFD, as V8 reads os.hostname()'s bytes:
  // the fingerprints are the reference's for these names (computed in Node).
  EXPECT_EQ(hostNameFrom("box\xff" "x"), "box\xEF\xBF\xBDx");
  EXPECT_EQ(keygrant::hostFingerprint(hostNameFrom("box\xff" "x"), "linux", "x64"),
            "7d6e273a64a7712e8c536b5ae4c43c8ee7ed664b751c422ead387f53f86967dd");
  EXPECT_EQ(keygrant::hostFingerprint(hostNameFrom("h\xE2\x82o\xED\xA0\x80st"), "linux", "x64"),
            "b1a61dea30e4fc2270d7d6e5529a560dd90beee08d805252aa0882decd71386e");
  // Well-formed names as they are.
  EXPECT_EQ(hostNameFrom("caf\xC3\xA9"), "caf\xC3\xA9");
  EXPECT_EQ(keygrant::hostFingerprint(hostNameFrom("caf\xC3\xA9"), "linux", "x64"),
            "b81ee4b091ec285fab13a26577c58c1305bccd3b0f31fd1914245dc2aef0343c");
  EXPECT_EQ(hostNameFrom("DESKTOP-7Q2"), "DESKTOP-7Q2");
}

/**
 * This machine's id as the reference reads it, by its own means: on Windows
 * `reg query` (its regular expression over the command's output), on macOS
 * `ioreg`; on Linux /etc/machine-id, judged by statfs and the boot time.
 * None when it cannot be told here (then nothing is asserted).
 */
std::optional<MachineIdReading> machineIdElsewhere() {
#if defined(_WIN32) || defined(__APPLE__)
#ifdef _WIN32
  FILE* pipe = _popen("reg query HKLM\\SOFTWARE\\Microsoft\\Cryptography /v MachineGuid /reg:64 2>NUL", "r");
  const std::regex pattern(R"(MachineGuid\s+REG_SZ\s+(\S+))");
#else
  FILE* pipe = popen("/usr/sbin/ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null", "r");
  const std::regex pattern(R"re("IOPlatformUUID"\s*=\s*"([^"]+)")re");
#endif
  if (pipe == nullptr) return std::nullopt;
  std::string output;
  char chunk[512];
  for (std::size_t got = 0; (got = fread(chunk, 1, sizeof chunk, pipe)) > 0;) output.append(chunk, got);
#ifdef _WIN32
  const int status = _pclose(pipe);
#else
  const int status = pclose(pipe);
#endif
  if (status != 0) return std::nullopt;
  std::smatch match;
  if (!std::regex_search(output, match, pattern)) return MachineIdReading::unavailable();
  return MachineIdReading::of(jsTrim(match[1].str()));
#elif defined(__linux__)
  const auto missing = [](const char* path) {
    struct stat info {};
    return stat(path, &info) != 0 && errno == ENOENT;
  };
  // Neither file there (a container, say): no id to read.
  if (missing("/etc/machine-id")) {
    if (missing("/var/lib/dbus/machine-id")) return MachineIdReading::unavailable();
    return std::nullopt;
  }
  std::ifstream file("/etc/machine-id");
  if (!file) return std::nullopt;
  std::stringstream text;
  text << file.rdbuf();
  const std::string id = jsTrim(text.str());
  if (id.empty()) return std::nullopt;
  // On a filesystem held in memory (tmpfs, ramfs): no id.
  struct statfs fsInfo {};
  if (statfs("/etc/machine-id", &fsInfo) != 0) return std::nullopt;
  const auto magic = static_cast<unsigned long>(fsInfo.f_type);
  if (magic == 0x01021994UL || magic == 0x858458F6UL) return MachineIdReading::unavailable();
  // Written at this boot (by the system's uptime): no id either.
  struct stat fileInfo {};
  struct sysinfo system {};
  if (stat("/etc/machine-id", &fileInfo) != 0 || sysinfo(&system) != 0) return std::nullopt;
  const std::int64_t bootedMs =
      (static_cast<std::int64_t>(std::time(nullptr)) - static_cast<std::int64_t>(system.uptime)) * 1000;
  const std::int64_t writtenMs = static_cast<std::int64_t>(fileInfo.st_mtime) * 1000;
  // Too close to the boot to tell by seconds either way: not judged here.
  if (std::llabs(writtenMs - (bootedMs - keygrant::kBootSlackMs)) < 3'000) return std::nullopt;
  if (writtenMs >= bootedMs - keygrant::kBootSlackMs) return MachineIdReading::unavailable();
  return MachineIdReading::of(id);
#else
  return std::nullopt;
#endif
}

TEST(SystemSource, ReadsThisMachinesIdAsTheReferenceWould) {
  const MachineIdReading reading = keygrant::readMachineId(keygrant::systemMachineIdSource(), Millis{5'000});
  ASSERT_NE(reading.kind, MachineIdReading::Kind::Transient);
  if (const auto expected = machineIdElsewhere()) {
    EXPECT_EQ(reading, *expected);
  }
  const auto identity = keygrant::SystemFingerprinter().identify(Millis{5'000});
  ASSERT_TRUE(identity);
  ASSERT_TRUE(identity->fingerprint);
  if (reading.kind == MachineIdReading::Kind::Id) {
    EXPECT_EQ(*identity, (keygrant::DeviceIdentity{keygrant::machineFingerprint(reading.id), DeviceKind::Machine,
                                                    *identity->fingerprint == fallback()
                                                        ? std::vector<std::string>{}
                                                        : std::vector<std::string>{fallback()}}));
  } else {
    EXPECT_EQ(*identity, (keygrant::DeviceIdentity{fallback(), DeviceKind::Hostname, {}}));
  }
}

}  // namespace
}  // namespace kgtest
