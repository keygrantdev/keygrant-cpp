// What the SDK sends and reads on the wire, and the small pieces of it that need no engine state.
#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "json_util.hpp"
#include "keygrant/types.hpp"

namespace keygrant::detail {

/// What the activate endpoint accepts for a device name, in UTF-16 units. Longer is cut, not refused.
inline constexpr std::size_t kMaxDeviceName = 120;
/// What it accepts for a licence key, in UTF-16 units.
inline constexpr std::size_t kMaxKey = 120;

/**
 * A product slug as it goes into a URL path, a file name and a registry key: 1
 * to 64 bytes of UTF-8, not `.` or `..`, with no white space, control
 * character, slash, backslash, `?`, `#` or `%`. Throws std::invalid_argument
 * for anything else.
 */
std::string checkedProduct(const std::string& product);

/**
 * A server's reply: its HTTP status, and its body as JSON (a discarded value
 * when it was not JSON). Moved, never copied: a body nests as deep as whoever
 * sent it likes, and copying a JSON value recurses as deep as it nests, deep
 * enough to overflow the stack.
 */
struct Reply {
  int status = 0;
  json body;

  Reply(int code, json value) : status(code), body(std::move(value)) {}
  Reply(Reply&&) = default;
  Reply& operator=(Reply&&) = default;
  Reply(const Reply&) = delete;
  Reply& operator=(const Reply&) = delete;
};

/// The server's error word: the body's `error` when the body is an object and it is a string.
std::optional<std::string> errorWord(const json& body);

/// An activation's error, from the server's error word.
ActivateError mapServerError(const std::optional<std::string>& error);

/// The definitive refusal a validate's error is, or none for any other error.
std::optional<Refusal> refusalOf(const std::optional<std::string>& error);

/**
 * The key a person entered, trimmed and upper-cased (the one sent and stored);
 * none when it is empty or longer than kMaxKey UTF-16 units.
 */
std::optional<std::string> enteredKey(const std::string& key);

/** Cut to `max` UTF-16 units without splitting a character (a high surrogate left alone is dropped). */
std::optional<std::string> trimToLength(const std::optional<std::string>& value, std::size_t max);

/**
 * A licensed status from the lease that licenses it: `test` when the lease says so, and the
 * entitlements it grants, if any. Test licences verify like any other (they are signed with the
 * product's real key); the flag is how an app tells them apart, e.g. to refuse them in a release
 * build.
 */
LicenseStatus licensedStatus(const std::optional<std::string>& key, bool test,
                             std::map<std::string, Entitlement> entitlements = {});

/** An invalid status: for a refusal (stored, or the server's), or a lease that does not license this build. */
LicenseStatus invalidStatus(LicenseReason reason, const std::optional<std::string>& key);

/** The status reason a refusal is given as. */
LicenseReason reasonOf(Refusal refusal);

/** The body's `lease` and `activationId`, kept only when they are strings. */
struct LeaseBody {
  std::optional<std::string> lease;
  std::optional<std::string> activationId;
};
LeaseBody parseLeaseBody(const json& body);

/** A trial answer: numeric `trialStart` and `trialEndsAt`, and a string `lease` when given. */
struct TrialBody {
  std::int64_t trialStart = 0;
  std::int64_t trialEndsAt = 0;
  std::optional<std::string> lease;
};
std::optional<TrialBody> parseTrialBody(const json& body);

/** The smallest of the times given, or none. */
std::optional<std::int64_t> earliest(std::initializer_list<std::optional<std::int64_t>> values);

/** Whether a response is no answer at all: an outage (5xx) or a refusal to serve now (429). */
inline bool noAnswer(int status) { return status >= 500 || status == 429; }

/** A string is "there" as JavaScript's truthiness has it: present and not empty. */
inline bool truthy(const std::optional<std::string>& value) { return value.has_value() && !value->empty(); }

}  // namespace keygrant::detail
