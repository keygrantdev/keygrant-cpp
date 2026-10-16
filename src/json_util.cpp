#include "json_util.hpp"

#include <cmath>

#include "utf.hpp"

namespace keygrant::detail {

namespace {

std::int64_t saturate(double value) {
  if (value >= static_cast<double>(kMaxSafeInteger)) return kMaxSafeInteger;
  if (value <= -static_cast<double>(kMaxSafeInteger)) return -kMaxSafeInteger;
  return static_cast<std::int64_t>(value);
}

/** Whether `text` is, whole, a JSON number (RFC 8259's grammar). */
bool isJsonNumber(std::string_view text) {
  std::size_t at = 0;
  const auto digits = [&] {
    const std::size_t from = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
    return at - from;
  };
  if (at < text.size() && text[at] == '-') ++at;
  if (at < text.size() && text[at] == '0') {
    ++at;
  } else if (digits() == 0) {
    return false;
  }
  if (at < text.size() && text[at] == '.') {
    ++at;
    if (digits() == 0) return false;
  }
  if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
    ++at;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
    if (digits() == 0) return false;
  }
  return at == text.size();
}

/**
 * `text` with every number outside a string that is out of a double's range (1e400) replaced by `null`; none when there
 * is no such number. JSON.parse reads such a number as Infinity and goes on, where the parser here refuses the whole
 * text. Infinity is no figure, count, time or major anything reads, so whatever member it is the value of is read as
 * absent, as a member of another shape is, and the text it rides in is read all the same.
 */
std::optional<std::string> clampInfiniteNumbers(const std::string& text) {
  std::string out;
  std::size_t copied = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    const char c = text[at];
    if (c == '"') {
      // Past the string, escapes included.
      ++at;
      while (at < text.size() && text[at] != '"') at += text[at] == '\\' ? 2 : 1;
      ++at;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      const std::size_t start = at;
      ++at;
      while (at < text.size() && ((text[at] >= '0' && text[at] <= '9') || text[at] == '+' || text[at] == '-' ||
                                  text[at] == '.' || text[at] == 'e' || text[at] == 'E')) {
        ++at;
      }
      const std::string_view token(text.data() + start, at - start);
      // Only a number with an exponent or past 308 digits can be out of range; the parser decides which are.
      const bool large = token.find_first_of("eE") != std::string_view::npos || token.size() > 308;
      if (large && isJsonNumber(token) && json::parse(std::string(token), nullptr, false).is_discarded()) {
        out.append(text, copied, start - copied);
        out.append("null");
        copied = at;
      }
    } else {
      ++at;
    }
  }
  if (copied == 0) return std::nullopt;
  out.append(text, copied, std::string::npos);
  return out;
}

}  // namespace

std::optional<std::int64_t> wholeNumber(const json& value) {
  if (value.is_number_unsigned()) {
    const auto n = value.get<std::uint64_t>();
    return n > static_cast<std::uint64_t>(kMaxSafeInteger) ? kMaxSafeInteger : static_cast<std::int64_t>(n);
  }
  if (value.is_number_integer()) {
    const auto n = value.get<std::int64_t>();
    if (n > kMaxSafeInteger) return kMaxSafeInteger;
    if (n < -kMaxSafeInteger) return -kMaxSafeInteger;
    return n;
  }
  if (value.is_number_float()) {
    const double d = value.get<double>();
    if (!std::isfinite(d) || std::floor(d) != d) return std::nullopt;
    return saturate(d);
  }
  return std::nullopt;
}

std::optional<std::int64_t> anyNumber(const json& value) {
  if (value.is_number_float()) {
    const double d = value.get<double>();
    if (std::isnan(d)) return std::nullopt;
    if (std::isinf(d)) return d > 0 ? kMaxSafeInteger : -kMaxSafeInteger;
    return saturate(std::floor(d));
  }
  return wholeNumber(value);
}

std::optional<std::int64_t> figureOf(const json& value) {
  if (value.is_number_unsigned()) {
    const auto n = value.get<std::uint64_t>();
    return n > static_cast<std::uint64_t>(kMaxSafeInteger) ? std::nullopt : std::optional<std::int64_t>(static_cast<std::int64_t>(n));
  }
  if (value.is_number_integer()) {
    const auto n = value.get<std::int64_t>();
    return n < 0 || n > kMaxSafeInteger ? std::nullopt : std::optional<std::int64_t>(n);
  }
  if (value.is_number_float()) {
    const double d = value.get<double>();
    // 2^53 - 1 is exact as a double, and so is every whole double below it.
    if (!std::isfinite(d) || d < 0 || std::floor(d) != d || d > static_cast<double>(kMaxSafeInteger)) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(d);
  }
  return std::nullopt;
}

std::optional<std::int64_t> countOf(const json& value) {
  const std::optional<std::int64_t> figure = figureOf(value);
  return figure && *figure >= 1 ? figure : std::nullopt;
}

std::optional<std::int64_t> figureMember(const json& value, const char* name) {
  if (!value.is_object()) return std::nullopt;
  const auto it = value.find(name);
  return it == value.end() ? std::nullopt : figureOf(*it);
}

bool trueMember(const json& value, const char* name) {
  if (!value.is_object()) return false;
  const auto it = value.find(name);
  return it != value.end() && it->is_boolean() && it->get<bool>();
}

json parseJsonText(std::string_view text) {
  const std::string decoded = decodeUtf8Text(text);
  json parsed = json::parse(decoded, nullptr, false);
  if (!parsed.is_discarded()) return parsed;
  // A number out of a double's range is Infinity to JSON.parse, and fails nothing there (clampInfiniteNumbers).
  const std::optional<std::string> clamped = clampInfiniteNumbers(decoded);
  if (!clamped) return parsed;
  return json::parse(*clamped, nullptr, false);
}

std::string dumpJson(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string dumpJson(const ordered_json& value) {
  return value.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
}

}  // namespace keygrant::detail
