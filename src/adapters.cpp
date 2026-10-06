// The small default adapters: the system clock, and the opt-in Windows stash.
#include "keygrant/adapters.hpp"

#include <cstring>
#include <vector>

#include "json_util.hpp"
#include "stash.hpp"
#include "sys.hpp"
#include "wire.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace keygrant {

namespace detail {

std::string trialStashKey(const std::string& product, const std::string& root) { return root + "\\" + product; }

std::optional<std::int64_t> trialStartFromQword(std::uint64_t value) {
  // 0 is no start, and from 2^53 a time is no longer exact in any SDK (a
  // QWORD of 2^63 or more would read as a negative start in a signed one).
  if (value == 0 || value > static_cast<std::uint64_t>(kMaxSafeInteger)) return std::nullopt;
  return static_cast<std::int64_t>(value);
}

std::optional<std::int64_t> trialStartFromDigits(std::u16string_view text) {
  while (!text.empty() && text.back() == u'\0') text.remove_suffix(1);
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  for (const char16_t c : text) {
    // Digits only: no sign, no white space, no other numerals.
    if (c < u'0' || c > u'9') return std::nullopt;
    value = value * 10 + static_cast<std::uint64_t>(c - u'0');
    // Stops before any overflow: past 2^53 it is no start whatever follows.
    if (value > static_cast<std::uint64_t>(kMaxSafeInteger)) return std::nullopt;
  }
  return trialStartFromQword(value);
}

bool stashableTrialStart(std::int64_t ms) { return ms > 0 && ms <= kMaxSafeInteger; }

}  // namespace detail

std::int64_t SystemClock::now() { return detail::wallClockMs(); }

#ifdef _WIN32

namespace {
const wchar_t kTrialStartValue[] = L"TrialStart";
/// The largest stash value read at all (bytes): far past any start's digits, however padded.
constexpr DWORD kLongestStashValue = 64 * 1024;
}

WindowsRegistryStash::WindowsRegistryStash(const std::string& product)
    : WindowsRegistryStash(product, detail::StashRoot{}) {}

WindowsRegistryStash::WindowsRegistryStash(const std::string& product, const detail::StashRoot& root)
    : keyUtf8_(detail::trialStashKey(detail::checkedProduct(product), root.subkey)), key_(detail::widen(keyUtf8_)) {}

std::optional<std::int64_t> WindowsRegistryStash::readTrialStart() {
  // Room for a QWORD or a start's digits at once; a longer value (digits padded
  // with zeros by a script, say) is read again into a buffer its size.
  std::vector<BYTE> data(256);
  DWORD type = REG_NONE;
  DWORD size = 0;
  LSTATUS status = ERROR_MORE_DATA;
  for (int attempt = 0; attempt < 3 && status == ERROR_MORE_DATA; ++attempt) {
    size = static_cast<DWORD>(data.size());
    status = RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), kTrialStartValue, RRF_RT_ANY | RRF_NOEXPAND, &type,
                          data.data(), &size);
    // Room for the NUL the registry adds to a string that has none; no more than a value could sensibly hold.
    if (status == ERROR_MORE_DATA && size <= kLongestStashValue) data.resize(size + sizeof(wchar_t));
  }
  if (status != ERROR_SUCCESS) return std::nullopt;
  if (type == REG_QWORD) {
    if (size != sizeof(std::uint64_t)) return std::nullopt;
    std::uint64_t value = 0;
    std::memcpy(&value, data.data(), sizeof value);
    return detail::trialStartFromQword(value);
  }
  if (type == REG_SZ) {
    std::u16string text(size / sizeof(char16_t), u'\0');
    std::memcpy(text.data(), data.data(), text.size() * sizeof(char16_t));
    return detail::trialStartFromDigits(text);
  }
  // Any other type is no start: a REG_DWORD read as a time would be 1970.
  return std::nullopt;
}

void WindowsRegistryStash::writeTrialStart(std::int64_t ms) {
  // Only what a reader takes: nothing is written that every SDK would not read back.
  if (!detail::stashableTrialStart(ms)) return;
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key_.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                      &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  const ULONGLONG value = static_cast<ULONGLONG>(ms);
  (void)RegSetValueExW(key, kTrialStartValue, 0, REG_QWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
  RegCloseKey(key);
}

#endif

}  // namespace keygrant
