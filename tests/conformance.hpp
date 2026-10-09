// What the conformance replays (conformance_test.cpp, conformance_purchase_test.cpp) share: a
// vector's JSON inputs read as the SDK's own types, and an answer written as the file writes it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "crypto.hpp"
#include "device.hpp"
#include "lease.hpp"
#include "machine_id.hpp"
#include "offline.hpp"
#include "renewal.hpp"
#include "support.hpp"
#include "wire.hpp"

namespace kgtest {

/** The conformance vectors' public key. */
const keygrant::detail::VerifyingKey& vectorsKey();

/** A group of the file, a test failure when it is missing or empty. */
const json& group(const char* name);

/** A vector's name (its JSON when it has none). */
std::string nameOf(const json& v);

std::optional<std::string> optString(const json& v);
std::optional<std::int64_t> optInt(const json& v);
json nullOr(const std::optional<std::string>& v);
json nullOr(const std::optional<std::int64_t>& v);

/** A state as JSON carries it (absent fields omitted). */
json stateJson(const StoredState& state);

/** A patch as JSON carries it: a cleared field is null. */
json patchJson(const keygrant::detail::StatePatch& p);

/** A patch from JSON: null clears the field; an absent field is left alone. */
keygrant::detail::StatePatch patchFrom(const json& j);

keygrant::detail::Asked askedFrom(const json& j);
keygrant::detail::Basis basisFrom(const json& j);

/** The vectors' base claims: a valid licence lease's claims, which the pure decisions read one or two of. */
keygrant::detail::LeaseClaims baseClaims();

/** A status as JSON says it: a member with no value left out, and entitlements only when there are any. */
json statusJson(const LicenseStatus& s);

/** What a status answer covers: its state, and whether it carried entitlements. */
std::string answerOf(const LicenseStatus& s);

json readingJson(const keygrant::MachineIdReading& r);

/** A device kind as JSON gives it: null is none. */
std::optional<DeviceKind> kindFrom(const json& v);

/** The claims a vector names, over the base claims. */
keygrant::detail::LeaseClaims claimsWith(const json& given);

keygrant::detail::LeaseVerdict verdictFrom(const std::string& word);

/** A lease's offline check as a vector gives it: {ok: true, test, claims}, {ok: false, reason}, or null for none. */
std::optional<keygrant::detail::LeaseCheck> checkFrom(const json& v);

/** A server's answer as a vector gives it: {status, json} (json null: no body), or null for none at all. */
std::optional<keygrant::detail::Reply> responseFrom(const json& v);

/** A renewal's outcome as JSON says it: the status as plain JSON, the patch as a patch. */
json renewalJson(const keygrant::detail::RenewalOutcome& outcome);

/** A device as JSON says it: no own or host name is null; no claim of its own, or no kind, is left out. */
json deviceJson(const keygrant::detail::Device& d);

/** A fingerprinter's answer as JSON says it: no fingerprint is null; no kind is left out. */
json identityJson(const keygrant::DeviceIdentity& identity);

}  // namespace kgtest
