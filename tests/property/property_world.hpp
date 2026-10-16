// The world a property test of use() runs the real License in (property_net.hpp is the machine; use_property_test.cpp the
// test): the steps of a case, and the oracle that judges every answer and what must hold between steps and at the end.
//
// The oracle is written from the contract and takes nothing from the code it judges but the License itself and the shape of a
// stored state: its constants, its figures and its waits are written again below, so a change to the code cannot move the code
// and what judges it together.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "journal.hpp"
#include "property/property_case.hpp"
#include "property/property_net.hpp"

namespace kgtest::property {

/// What the state said of a spend before the call.
struct Pre {
  bool licensed = false;
  /// The device holds figures for the meter: the key has it. A meter it holds none for is one the server does not know either.
  bool known = false;
  bool covered = false;
  /// What use() must answer as `left` for a spend made offline: the figure less the unsynced, less the spend.
  std::int64_t leftAfter = 0;
  std::size_t entries = 0;
  bool openOffline = false;
  /// A wait of the covered spends runs: the no-answer wait (device clock) or the journal's (guarded clock).
  bool waiting = false;
  /// The wait of a spend the allowance refuses runs.
  bool voidWaiting = false;
};

/// A spend refused after the server recorded it: the take-back is the void's. `full`: the journal was full when the spend was
/// made, so the void may have found no room.
struct Phantom {
  std::string id;
  std::string meter;
  std::int64_t n = 0;
  bool full = false;
};

class World {
 public:
  World(Case c, bool tracing);
  ~World();
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  /// Steps the case and judges it; throws a PropertyFailure for the first property that did not hold.
  void run();

  /// What happened, for reading a failing case.
  std::vector<std::string> trace();

 private:
  std::shared_ptr<keygrant::License> proc(int p);
  std::vector<std::shared_ptr<keygrant::License>> procs();
  [[noreturn]] void fail(const std::string& invariant, const std::string& message);
  bool tracing();
  void note(std::string line);
  std::string lbl(const std::string& id);
  StoredState current();
  std::vector<keygrant::detail::JournalOp> held(int p);
  std::vector<keygrant::SpendEntry> viewEntries(int p);

  void step(const Step& s);
  void setDraws(const std::vector<double>& r);
  std::shared_ptr<Running> launchStatus(int p, bool force);
  void expectStatus(const Running& running);
  void pass(std::int64_t ms);
  void deactivate(int p);
  void lose(const std::vector<keygrant::detail::JournalOp>& ops);
  void restart(int p);
  void ensureActive();
  Pre preOf(const Call& call);
  void useCalls(const std::vector<Call>& calls, const std::optional<std::pair<int, bool>>& side);
  void judge(const Call& call, const Pre& pre, const keygrant::UseResult& res, const std::vector<Req>& reqs, bool concurrent,
             bool risky, const std::vector<keygrant::SpendEntry>& view);
  void describe();
  void accountVoids();
  std::vector<Phantom> outstanding();
  void check();
  void converge();

  Case case_;
  std::shared_ptr<Shared> shared_;
  std::size_t stepNo_ = 0;
  /// Every use() the customer was told was made, by meter.
  std::map<std::string, std::int64_t> acked_;
  /// What the customer was told was made while only memory held it and has not been reported since, by meter: the overage the
  /// allowance may show.
  std::map<std::string, std::int64_t> told_;
  std::size_t toldOps_ = 0;
  /// What the contract allows to be lost unreported, by meter.
  std::map<std::string, std::int64_t> lossAllowed_;
  std::vector<Phantom> phantoms_;
  /// The ids of the voids each process held in memory at the last check.
  std::set<std::string> heldVoids_[2];
};

/// Run a case: none when every property held. With `trace` the trace of what happened is given too.
std::pair<std::optional<PropertyFailure>, std::vector<std::string>> runCase(const Case& c, bool trace);

}  // namespace kgtest::property
