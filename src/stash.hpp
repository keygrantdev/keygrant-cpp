// The default Windows trial stash's one format, shared by every KeyGrant SDK
// (docs/specs/sdk-contract.md 5.4): HKCU\Software\KeyGrant\<product>, value
// TrialStart, the start in ms since the epoch as a REG_QWORD. What a reader
// takes of it is decided here, without the registry, so every platform tests it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace keygrant::detail {

/// The stash's value name.
inline constexpr const char* kTrialStartValueName = "TrialStart";

/// The key under HKEY_CURRENT_USER that every product's stash key is below.
inline constexpr const char* kTrialStashRoot = "Software\\KeyGrant";

/// The stash's key under HKEY_CURRENT_USER for a product: below the shared root, or (tests) one of their own.
std::string trialStashKey(const std::string& product, const std::string& root = kTrialStashRoot);

/** A trial start read as a REG_QWORD: a time in (0, 2^53), else none. */
std::optional<std::int64_t> trialStartFromQword(std::uint64_t value);

/**
 * A trial start read as a REG_SZ (what a script or a hand writes): decimal
 * digits only, after the terminating NULs the registry may keep, and a time
 * in (0, 2^53); anything else is none.
 */
std::optional<std::int64_t> trialStartFromDigits(std::u16string_view text);

/** Whether a start is one a reader takes: what the stash writes, and nothing else. */
bool stashableTrialStart(std::int64_t ms);

}  // namespace keygrant::detail
