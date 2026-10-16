// JSON values read as the reference's JavaScript reads them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace keygrant::detail {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

/**
 * The largest magnitude an integer is held to: JavaScript's safe integers
 * (2^53 - 1). Larger ones are saturated to it, which keeps every comparison
 * with a real time or major as JavaScript makes it, and leaves room to
 * multiply a time in seconds by 1000 without overflow.
 */
inline constexpr std::int64_t kMaxSafeInteger = 9'007'199'254'740'991;

/// A JSON number that is a whole number (Number.isInteger), saturated to the safe range; otherwise none.
std::optional<std::int64_t> wholeNumber(const json& value);

/// Any finite JSON number (typeof 'number'), floored and saturated to the safe range; otherwise none.
std::optional<std::int64_t> anyNumber(const json& value);

/**
 * A figure of a meter: a JSON number that is a non-negative whole number of at most 2^53 - 1, however it is written
 * (4.0 is 4, 1e1 is 10, -0 is 0). A fraction, a negative, a number out of a double's range, one past 2^53 - 1, or
 * anything that is not a number is none: unlike wholeNumber it saturates nothing.
 */
std::optional<std::int64_t> figureOf(const json& value);

/// A count: a figure of at least 1.
std::optional<std::int64_t> countOf(const json& value);

/// The member `name` of `value` read as a figure: none when `value` is no object, the member is missing or is no figure.
std::optional<std::int64_t> figureMember(const json& value, const char* name);

/// Whether the member `name` of `value` is JSON true: a text, a number, null, a missing member and a non-object are not.
bool trueMember(const json& value, const char* name);

/**
 * A body as JSON.parse reads the text a server sent (UTF-8 decoded, a BOM
 * dropped, a number out of a double's range, which JSON.parse reads as Infinity, read as null: the member it is the
 * value of is read as absent, and the rest of the text is read all the same); a discarded value when it is not JSON.
 * Never throws.
 */
json parseJsonText(std::string_view text);

/**
 * JSON text that never throws for its content: invalid UTF-8 in a string is
 * replaced. Writing JSON recurses as deep as the value nests: never given a
 * value someone else built without a depth check first (nestsDeeperThan).
 */
std::string dumpJson(const json& value);
std::string dumpJson(const ordered_json& value);

/**
 * Whether a value nests deeper than `limit` (a scalar nests 0 deep, `[]` and
 * `{}` 1), found without recursion: copying or writing a JSON value recurses
 * as deep as it nests, and one parsed from a file or a server can nest deep
 * enough to overflow the stack.
 */
template <class Json>
bool nestsDeeperThan(const Json& value, std::size_t limit) {
  // Depth first, on a stack of our own rather than the call stack.
  std::vector<std::pair<const Json*, std::size_t>> pending{{&value, 0}};
  while (!pending.empty()) {
    const auto [node, depth] = pending.back();
    pending.pop_back();
    if (!node->is_structured()) continue;
    if (depth + 1 > limit) return true;
    for (const Json& child : *node) pending.emplace_back(&child, depth + 1);
  }
  return false;
}

}  // namespace keygrant::detail
