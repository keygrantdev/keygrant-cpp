// A case of the property test of use(): plain data, so a failing one can be shrunk and kept as a named test. The generator is
// the reference's, line for line (the same seeded generator and the draws in the same order), so a seed means the same case in
// every KeyGrant SDK.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "keygrant/meter.hpp"

namespace kgtest::property {

using json = nlohmann::json;

inline constexpr std::int64_t kMinute = 60'000;
inline constexpr std::int64_t kHour = 3'600'000;

/// A meter the key holds.
struct MeterSpec {
  std::int64_t total = 0;
  std::int64_t offline = 0;
  bool resets = false;

  bool operator==(const MeterSpec& other) const {
    return total == other.total && offline == other.offline && resets == other.resets;
  }
};

/// One use() of a process.
struct Call {
  int p = 0;
  std::string meter;
  std::int64_t n = 1;
};

/// An entry the journal already holds when the case begins. `recorded`: the server holds the spend (or the report) already.
struct Preloaded {
  std::string meter;
  std::int64_t n = 1;
  keygrant::SpendKind kind = keygrant::SpendKind::Unanswered;
  std::int64_t agoMs = 0;
  bool recorded = false;
};

/// What a step does; `r` are the random numbers its requests draw their fates from, so a case replays whole.
struct Step {
  enum class Kind { Use, Status, Repeat, Deactivate, Restart, Wait, Jump, Net, Disk };

  Kind kind = Kind::Wait;
  std::vector<Call> calls;                   // Use
  std::optional<std::pair<int, bool>> side;  // Use: a status beside the calls: process, force
  int p = 0;                                 // Status, Deactivate, Restart
  bool force = false;                        // Status
  Call call;                                 // Repeat
  std::int64_t times = 0;                    // Repeat
  std::int64_t gapMs = 0;                    // Repeat
  std::int64_t ms = 0;                       // Wait, Jump
  std::string mode;                          // Net: good, down, flaky
  bool failing = false;                      // Disk
  std::vector<double> r;                     // Use, Status, Repeat, Deactivate

  /// The step's kind, as the file writes it.
  const char* name() const;
  json toJson() const;
  static Step fromJson(const json& value);
};

/// A case: the meters, the journal it begins with, and the steps.
struct Case {
  std::int64_t seed = 0;
  std::map<std::string, MeterSpec> meters;
  std::vector<Preloaded> preload;
  std::vector<Step> steps;

  json toJson() const;
  static Case fromJson(const json& value);
  Case withSteps(std::vector<Step> steps) const;
};

/// The reference's seeded generator (mulberry32): the same seed, the same sequence.
class Rng {
 public:
  explicit Rng(std::uint32_t seed) : state_(seed) {}
  double next();

 private:
  std::uint32_t state_;
};

/// The case a seed makes: the reference's generator, line for line.
Case generate(std::int64_t seed);

}  // namespace kgtest::property
