// The groups of conformance/vectors.json after the first ones, replayed as the reference answers them
// from the JSON: every answer must match.
#include <cmath>

#include "conformance.hpp"
#include "offline.hpp"
#include "url.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;

TEST(ConformanceLater, Majors) {
  int replayed = 0;
  for (const json& v : group("majors")) {
    SCOPED_TRACE(nameOf(v));
    const json& configured = v["configured"];
    // A configured major here is a whole number (std::int64_t, signed): a
    // fraction is one no config of this SDK can hold, and only a vector marked
    // jsOnly may be one. A negative is whole, and replayed.
    if (configured.is_number_float() && std::trunc(configured.get<double>()) != configured.get<double>()) {
      EXPECT_TRUE(v.value("jsOnly", false)) << "a fraction not marked jsOnly";
      continue;
    }
    const std::optional<std::int64_t> major =
        configured.is_null() ? std::nullopt : std::optional<std::int64_t>(configured.get<std::int64_t>());
    EXPECT_EQ(majorOf(major), v["expect"].get<std::int64_t>());
    ++replayed;
  }
  // None, 0, 1, 2, 7 and a negative, at least.
  EXPECT_GE(replayed, 6);
}

TEST(ConformanceLater, ReferencedLinks) {
  int replayed = 0;
  for (const json& v : group("referencedLinks")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(referencedLink(v["link"].get<std::string>(), v["reference"].get<std::string>()),
              v["expect"].get<std::string>());
    ++replayed;
  }
  EXPECT_GE(replayed, 1);
}

TEST(ConformanceLater, TrialReportDue) {
  for (const json& v : group("trialReportDue")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(trialReportDue(stateFrom(v["state"]), v["now"].get<std::int64_t>()), v["expect"].get<bool>());
  }
}

}  // namespace
}  // namespace kgtest
