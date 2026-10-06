// The Windows trial stash's one format (docs/specs/sdk-contract.md 5.4): what a
// reader takes, decided on every platform, and the registry itself on Windows.
#include <cstdint>
#include <limits>
#include <string>

#include "json_util.hpp"
#include "stash.hpp"
#include "support.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kgtest {
namespace {

using keygrant::detail::kMaxSafeInteger;
using keygrant::detail::stashableTrialStart;
using keygrant::detail::trialStartFromDigits;
using keygrant::detail::trialStartFromQword;

constexpr std::uint64_t kTwoTo53 = 9'007'199'254'740'992ULL;

/** UTF-16 text as a failure message shows it: printable ASCII as itself, anything else as \uXXXX. */
template <class Char>
std::string shown(const std::basic_string<Char>& text) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (const Char c : text) {
    const auto unit = static_cast<std::uint32_t>(c);
    if (unit >= 0x20 && unit < 0x7F) {
      out.push_back(static_cast<char>(unit));
    } else {
      out += "\\u";
      for (int shift = 12; shift >= 0; shift -= 4) out.push_back(hex[(unit >> shift) & 0xF]);
    }
  }
  return out;
}

TEST(TrialStash, IsKeptUnderTheProductsOwnKey) {
  EXPECT_EQ(keygrant::detail::trialStashKey("sluice"), "Software\\KeyGrant\\sluice");
  EXPECT_STREQ(keygrant::detail::kTrialStartValueName, "TrialStart");
}

TEST(TrialStash, ReadsAQwordInsideTheSafeRangeAsTheStart) {
  EXPECT_EQ(trialStartFromQword(1'790'000'000'000ULL), 1'790'000'000'000);
  EXPECT_EQ(trialStartFromQword(1), 1);
  EXPECT_EQ(trialStartFromQword(kTwoTo53 - 1), kMaxSafeInteger);
  // 0 is no start; from 2^53 a time is no longer exact; a QWORD of 2^63 or
  // more would be a negative start, which the server refuses as a clientStart.
  EXPECT_EQ(trialStartFromQword(0), std::nullopt);
  EXPECT_EQ(trialStartFromQword(kTwoTo53), std::nullopt);
  EXPECT_EQ(trialStartFromQword(1ULL << 63), std::nullopt);
  EXPECT_EQ(trialStartFromQword(std::numeric_limits<std::uint64_t>::max()), std::nullopt);
}

TEST(TrialStash, ReadsAStringOfDecimalDigitsAndNothingElse) {
  EXPECT_EQ(trialStartFromDigits(u"1790000000000"), 1'790'000'000'000);
  // The registry keeps a REG_SZ's terminating NULs.
  EXPECT_EQ(trialStartFromDigits(std::u16string(u"1790000000000") + u'\0'), 1'790'000'000'000);
  EXPECT_EQ(trialStartFromDigits(std::u16string(u"1790000000000") + u'\0' + u'\0'), 1'790'000'000'000);
  EXPECT_EQ(trialStartFromDigits(u"0001790000000000"), 1'790'000'000'000);
  EXPECT_EQ(trialStartFromDigits(std::u16string(300, u'0') + u"1790000000000"), 1'790'000'000'000);
  EXPECT_EQ(trialStartFromDigits(u"9007199254740991"), kMaxSafeInteger);
  for (const std::u16string& text : {std::u16string(u""), std::u16string(1, u'\0'), std::u16string(u"0"),
                                     std::u16string(u"000"), std::u16string(u"-1790000000000"),
                                     std::u16string(u"+1790000000000"), std::u16string(u" 1790000000000"),
                                     std::u16string(u"1790000000000 "), std::u16string(u"1790000000000\n"),
                                     std::u16string(u"1.79e12"), std::u16string(u"1790000000000.0"),
                                     std::u16string(u"0x1A0C"), std::u16string(u"9007199254740992"),
                                     std::u16string(u"18446744073709551616"),
                                     std::u16string(u"99999999999999999999999999999999"),
                                     // Arabic-Indic and full-width digits are digits elsewhere, not here.
                                     std::u16string(u"\u0661\u0667\u0669"), std::u16string(u"\uFF11\uFF17\uFF19"),
                                     std::u16string(u"179") + u'\0' + u"0000000000"}) {
    EXPECT_EQ(trialStartFromDigits(text), std::nullopt) << shown(text);
  }
}

TEST(TrialStash, WritesOnlyAStartEveryReaderTakesBack) {
  EXPECT_TRUE(stashableTrialStart(1'790'000'000'000));
  EXPECT_TRUE(stashableTrialStart(1));
  EXPECT_TRUE(stashableTrialStart(kMaxSafeInteger));
  EXPECT_FALSE(stashableTrialStart(0));
  EXPECT_FALSE(stashableTrialStart(-1));
  EXPECT_FALSE(stashableTrialStart(kMaxSafeInteger + 1));
  EXPECT_FALSE(stashableTrialStart(std::numeric_limits<std::int64_t>::min()));
}

#ifdef _WIN32

/**
 * The stash below a root of the test's own, HKCU\Software\KeyGrant-test-<pid>,
 * removed afterwards with everything under it: never HKCU\Software\KeyGrant,
 * where a real app's stash may be.
 */
class RegistryStash : public ::testing::Test {
 protected:
  void SetUp() override {
    root_.subkey = "Software\\KeyGrant-test-" + std::to_string(GetCurrentProcessId());
    rootKey_ = std::wstring(root_.subkey.begin(), root_.subkey.end());
    key_ = rootKey_ + L"\\" + std::wstring(product_.begin(), product_.end());
    RegDeleteTreeW(HKEY_CURRENT_USER, rootKey_.c_str());
  }

  void TearDown() override {
    RegDeleteTreeW(HKEY_CURRENT_USER, rootKey_.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, rootKey_.c_str());
  }

  /** The stash under test: the product's, below the test's own root. */
  keygrant::WindowsRegistryStash stash() const { return keygrant::WindowsRegistryStash(product_, root_); }

  void put(DWORD type, const void* data, DWORD size) {
    ASSERT_EQ(RegSetKeyValueW(HKEY_CURRENT_USER, key_.c_str(), L"TrialStart", type, data, size), ERROR_SUCCESS);
  }
  void putQword(std::uint64_t value) { put(REG_QWORD, &value, sizeof value); }
  void putString(DWORD type, const std::wstring& text) {
    put(type, text.c_str(), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
  }

  /** What is stored, as its type and its QWORD (when it is one). */
  std::optional<std::pair<DWORD, std::uint64_t>> stored() {
    DWORD type = REG_NONE;
    std::uint64_t value = 0;
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), L"TrialStart", RRF_RT_ANY, &type, &value, &size) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    return std::make_pair(type, value);
  }

  const std::string product_ = "sluice";
  keygrant::detail::StashRoot root_;
  std::wstring rootKey_;
  std::wstring key_;
};

TEST(TrialStashKey, IsTheSharedOneForAnAppWhateverRootTheTestsUse) {
  // Computed, never written: the stash an app makes keeps the one shared format's key.
  EXPECT_EQ(keygrant::WindowsRegistryStash("sluice").key(), "Software\\KeyGrant\\sluice");
  EXPECT_EQ(keygrant::detail::StashRoot{}.subkey, keygrant::detail::kTrialStashRoot);
  keygrant::detail::StashRoot own;
  own.subkey = "Software\\KeyGrant-test-1";
  EXPECT_EQ(keygrant::WindowsRegistryStash("sluice", own).key(), "Software\\KeyGrant-test-1\\sluice");
}

TEST_F(RegistryStash, ReadsAQwordInsideTheSafeRange) {
  keygrant::WindowsRegistryStash stash = this->stash();
  EXPECT_EQ(stash.readTrialStart(), std::nullopt);
  putQword(1'790'000'000'000ULL);
  EXPECT_EQ(stash.readTrialStart(), 1'790'000'000'000);
  putQword(kTwoTo53 - 1);
  EXPECT_EQ(stash.readTrialStart(), kMaxSafeInteger);
  for (const std::uint64_t outside : {std::uint64_t{0}, kTwoTo53, std::uint64_t{1} << 63,
                                      std::numeric_limits<std::uint64_t>::max()}) {
    putQword(outside);
    EXPECT_EQ(stash.readTrialStart(), std::nullopt) << outside;
  }
}

TEST_F(RegistryStash, ReadsAStringOfDecimalDigits) {
  keygrant::WindowsRegistryStash stash = this->stash();
  putString(REG_SZ, L"1790000000000");
  EXPECT_EQ(stash.readTrialStart(), 1'790'000'000'000);
  putString(REG_SZ, L"0001790000000000");
  EXPECT_EQ(stash.readTrialStart(), 1'790'000'000'000);
  putString(REG_SZ, L"9007199254740991");
  EXPECT_EQ(stash.readTrialStart(), kMaxSafeInteger);
  // Digits padded with zeros however far (longer than the first read's buffer).
  putString(REG_SZ, std::wstring(300, L'0') + L"1790000000000");
  EXPECT_EQ(stash.readTrialStart(), 1'790'000'000'000);
  for (const wchar_t* text : {L"", L"0", L"-1790000000000", L" 1790000000000", L"1790000000000 ", L"1.79e12",
                              L"9007199254740992", L"18446744073709551616", L"next tuesday"}) {
    putString(REG_SZ, text);
    EXPECT_EQ(stash.readTrialStart(), std::nullopt) << shown(std::wstring(text));
  }
  // Long, and not a start for it: past 2^53 whatever its length.
  putString(REG_SZ, std::wstring(300, L'9'));
  EXPECT_EQ(stash.readTrialStart(), std::nullopt);
}

TEST_F(RegistryStash, ReadsAnyOtherTypeAsNoStart) {
  keygrant::WindowsRegistryStash stash = this->stash();
  const DWORD dword = 1'790'000'000;
  put(REG_DWORD, &dword, sizeof dword);
  EXPECT_EQ(stash.readTrialStart(), std::nullopt);
  putString(REG_EXPAND_SZ, L"1790000000000");
  EXPECT_EQ(stash.readTrialStart(), std::nullopt);
  const wchar_t multi[] = L"1790000000000\0";
  put(REG_MULTI_SZ, multi, sizeof multi);
  EXPECT_EQ(stash.readTrialStart(), std::nullopt);
  const std::uint64_t binary = 1'790'000'000'000ULL;
  put(REG_BINARY, &binary, sizeof binary);
  EXPECT_EQ(stash.readTrialStart(), std::nullopt);
  // A QWORD's type with a DWORD's size.
  put(REG_QWORD, &dword, sizeof dword);
  EXPECT_EQ(stash.readTrialStart(), std::nullopt);
}

TEST_F(RegistryStash, WritesTheStartAsAQwordAndReadsItBack) {
  keygrant::WindowsRegistryStash stash = this->stash();
  stash.writeTrialStart(1'790'000'000'000);
  EXPECT_EQ(stored(), std::make_pair(DWORD{REG_QWORD}, std::uint64_t{1'790'000'000'000ULL}));
  EXPECT_EQ(stash.readTrialStart(), 1'790'000'000'000);
  // Another stash for the same product (another SDK, another run) sees it.
  EXPECT_EQ(this->stash().readTrialStart(), 1'790'000'000'000);
}

TEST_F(RegistryStash, WritesNothingAReaderWouldNotTakeBack) {
  keygrant::WindowsRegistryStash stash = this->stash();
  for (const std::int64_t outside : {std::int64_t{0}, std::int64_t{-1}, kMaxSafeInteger + 1,
                                     std::numeric_limits<std::int64_t>::max(),
                                     std::numeric_limits<std::int64_t>::min()}) {
    stash.writeTrialStart(outside);
    EXPECT_EQ(stored(), std::nullopt) << outside;
  }
}

TEST_F(RegistryStash, IsForAProductSlugOnly) {
  EXPECT_THROW(keygrant::WindowsRegistryStash("Software\\KeyGrant\\sluice"), std::invalid_argument);
  EXPECT_THROW(keygrant::WindowsRegistryStash(""), std::invalid_argument);
  EXPECT_THROW(keygrant::WindowsRegistryStash(".."), std::invalid_argument);
}

#endif

}  // namespace
}  // namespace kgtest
