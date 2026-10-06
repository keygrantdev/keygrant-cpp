// The running system's names and machine id. The Windows and macOS parts are
// small reads against documented APIs; everything they return is classified
// by the portable functions in machine_id.cpp, which are tested everywhere.
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "keygrant/fingerprint.hpp"
#include "keygrant/storage.hpp"
#include "machine_id.hpp"
#include "sys.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#endif
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace keygrant {

std::string platformName() {
#if defined(_WIN32)
  return "win32";
#elif defined(__APPLE__)
  return "darwin";
#elif defined(__ANDROID__)
  return "android";
#elif defined(__linux__)
  return "linux";
#elif defined(__FreeBSD__)
  return "freebsd";
#elif defined(__OpenBSD__)
  return "openbsd";
#elif defined(__NetBSD__)
  return "netbsd";
#elif defined(__sun)
  return "sunos";
#elif defined(_AIX)
  return "aix";
#else
  return "linux";
#endif
}

std::string archName() {
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
  return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
  return "ia32";
#elif defined(_M_ARM) || defined(__arm__)
  return "arm";
#elif defined(__powerpc64__)
  return "ppc64";
#elif defined(__s390x__)
  return "s390x";
#elif defined(__riscv) && defined(__riscv_xlen) && __riscv_xlen == 64
  return "riscv64";
#elif defined(__loongarch64)
  return "loong64";
#else
  return "unknown";
#endif
}

#ifdef _WIN32

std::optional<std::string> hostName() {
  // As Node reads it (libuv): GetHostNameW, looked up at run time as libuv does.
  static std::once_flag started;
  std::call_once(started, [] {
    WSADATA data;
    (void)WSAStartup(MAKEWORD(2, 2), &data);
  });
  using GetHostNameWFn = int(WSAAPI*)(PWSTR, int);
  const HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
  const FARPROC proc = ws2 != nullptr ? GetProcAddress(ws2, "GetHostNameW") : nullptr;
  if (proc != nullptr) {
    const auto getHostNameW = reinterpret_cast<GetHostNameWFn>(reinterpret_cast<void (*)(void)>(proc));
    wchar_t buffer[257] = {};
    if (getHostNameW(buffer, 257) == 0) return detail::hostNameFrom(detail::narrow(buffer));
    return std::nullopt;
  }
  char narrowBuffer[257] = {};
  if (gethostname(narrowBuffer, 256) != 0) return std::nullopt;
  return detail::hostNameFrom(narrowBuffer);
}

#else

std::optional<std::string> hostName() {
  char buffer[257] = {};
  if (gethostname(buffer, 256) != 0) return std::nullopt;
  buffer[256] = '\0';
  // The bytes the system holds, read as Node reads them: never hashed raw.
  return detail::hostNameFrom(buffer);
}

#endif

namespace {

#ifdef _WIN32

/** MachineGuid by RegGetValueW, in the 64-bit view whatever this process is. */
RegistryRead readGuid() {
  static const wchar_t kKey[] = L"SOFTWARE\\Microsoft\\Cryptography";
  static const wchar_t kValue[] = L"MachineGuid";
  std::vector<wchar_t> buffer(128);
  bool viaKey = false;
  for (int attempt = 0; attempt < 4; ++attempt) {
    DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    LSTATUS status = ERROR_SUCCESS;
    if (!viaKey) {
      status = RegGetValueW(HKEY_LOCAL_MACHINE, kKey, kValue, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr,
                            buffer.data(), &bytes);
      // A Windows that does not know RRF_SUBKEY_WOW6464KEY: open the key in the 64-bit view instead.
      if (status == ERROR_INVALID_PARAMETER) {
        viaKey = true;
        continue;
      }
    } else {
      HKEY key = nullptr;
      status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
      if (status == ERROR_SUCCESS) {
        status = RegGetValueW(key, nullptr, kValue, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes);
        RegCloseKey(key);
      }
    }
    if (status == ERROR_MORE_DATA) {
      buffer.resize(bytes / sizeof(wchar_t) + 2);
      continue;
    }
    if (status != ERROR_SUCCESS) return RegistryRead{static_cast<long>(status), {}};
    const std::size_t units = std::min<std::size_t>(bytes / sizeof(wchar_t), buffer.size());
    std::u16string data;
    data.reserve(units);
    for (std::size_t i = 0; i < units; ++i) data.push_back(static_cast<char16_t>(buffer[i]));
    return RegistryRead{0, data};
  }
  return RegistryRead{static_cast<long>(ERROR_MORE_DATA), {}};
}

#endif

#ifdef __APPLE__

/** IOPlatformUUID of the IOPlatformExpertDevice service, by IOKit: no subprocess. */
IoKitRead readUuid() {
  CFMutableDictionaryRef matching = IOServiceMatching("IOPlatformExpertDevice");
  if (matching == nullptr) return IoKitRead{IoKitRead::Status::Failed, {}};
  // Consumes `matching`. MACH_PORT_NULL is the default main port on every macOS.
  const io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, matching);
  if (service == IO_OBJECT_NULL) return IoKitRead{IoKitRead::Status::NoService, {}};
  const CFTypeRef value = IORegistryEntryCreateCFProperty(service, CFSTR("IOPlatformUUID"), kCFAllocatorDefault, 0);
  IOObjectRelease(service);
  if (value == nullptr) return IoKitRead{IoKitRead::Status::NoProperty, {}};
  if (CFGetTypeID(value) != CFStringGetTypeID()) {
    CFRelease(value);
    return IoKitRead{IoKitRead::Status::NotAString, {}};
  }
  const auto string = static_cast<CFStringRef>(value);
  const CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<std::size_t>(size), '\0');
  const Boolean converted = CFStringGetCString(string, out.data(), size, kCFStringEncodingUTF8);
  CFRelease(value);
  if (!converted) return IoKitRead{IoKitRead::Status::Failed, {}};
  out.resize(std::strlen(out.c_str()));
  return IoKitRead{IoKitRead::Status::Ok, out};
}

#endif

/** The running system. */
class SystemMachineIdSource final : public MachineIdSource {
 public:
  std::string platform() override { return platformName(); }

  RegistryRead readMachineGuid() override {
#ifdef _WIN32
    return readGuid();
#else
    return RegistryRead{detail::kWinFileNotFound, {}};
#endif
  }

  IoKitRead readPlatformUuid() override {
#ifdef __APPLE__
    return readUuid();
#else
    return IoKitRead{IoKitRead::Status::NoService, {}};
#endif
  }

  std::string readFile(const std::string& path) override { return systemStorageFs()->readFile(path); }

  std::optional<std::int64_t> mtimeMs(const std::string& path) override { return systemStorageFs()->mtimeMs(path); }
};

}  // namespace

std::shared_ptr<MachineIdSource> systemMachineIdSource() {
  // Never destroyed: an abandoned read may still be running at exit.
  static const auto* source = new std::shared_ptr<MachineIdSource>(std::make_shared<SystemMachineIdSource>());
  return *source;
}

}  // namespace keygrant
