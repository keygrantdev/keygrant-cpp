// The server a property test of use() runs the real License against: what the KeyGrant API does with a request that carries a
// spend or a report of unsynced ones, as the contract says: every report lands once by its id, an entry that grew adds what it
// grew by, a void takes an online spend or a report back and keeps its id from counting later, and an answer names the entries
// it holds. Test support only.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "property/property_case.hpp"
#include "support.hpp"

namespace kgtest::property {

/// What the server did with one request.
struct Served {
  int status = 200;
  json body;
  /// The id of a spend of its own that this request recorded: a `use` that landed.
  std::optional<std::string> spent;
};

/// What it holds by spend id: an online spend or a report, at the count it has.
struct Held {
  std::string meter;
  std::int64_t n = 0;
};

class PropServer {
 public:
  PropServer(std::map<std::string, MeterSpec> meters, TestKey key, std::int64_t now) : meters(std::move(meters)), now(now), key_(std::move(key)) {}

  std::map<std::string, MeterSpec> meters;
  /// What it holds by spend id.
  std::map<std::string, Held> recorded;
  /// The ids a void took back, or was the first to name.
  std::set<std::string> voided;
  /// The server's time (ms): it moves on by a millisecond with each request, so no two answers carry one time.
  std::int64_t now = 0;

  /// What a meter has used: its recorded spends, whatever the allowance.
  std::int64_t used(const std::string& code) const;
  std::int64_t left(const std::string& code) const;

  /// The lease the server signs now for an activation: the key's meters as they stand, valid for 60 days.
  std::string lease(const std::string& aid) const;

  /// What the server answers a request of `action`, and what it recorded.
  Served handle(const std::string& action, const json& body);

 private:
  json figures() const;
  json report(const json& spends);
  Served useMeter(const json& body, const json& named);

  TestKey key_;
  int activations_ = 0;
};

}  // namespace kgtest::property
