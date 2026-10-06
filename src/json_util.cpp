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

json parseJsonText(std::string_view text) {
  const std::string decoded = decodeUtf8Text(text);
  return json::parse(decoded, nullptr, false);
}

std::string dumpJson(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string dumpJson(const ordered_json& value) {
  return value.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
}

}  // namespace keygrant::detail
