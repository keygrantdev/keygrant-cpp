// How the journal of unsynced spends (StoredState::spends) and the figures beside it (StoredState::meterFigures) land on
// the stored state: on the state as stored at that moment, as `merged` lands the rest, so that two processes of one app
// never lose each other's spends. Both fields are about the stored activation: a save made for another activation, or on
// a state with none, writes neither.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "keygrant/types.hpp"
#include "meters.hpp"

namespace keygrant::detail {

/**
 * One change to the journal or its figures, about the activation `about`.
 *
 *  - A spend (`Offline`, `Unanswered`, `Void`): a spend made with no request sent (`Offline`), or whose own request got
 *    no answer (`Unanswered`), or one refused after that (`Void`). A spend carries the most its meter's unsynced spends
 *    may come to with it (`limit`, the smaller of the device's allowance and what is left), judged again where it
 *    lands. `told`: the customer was told the spend was made while a failed save kept it in memory, so it lands
 *    whatever the limit and the cap say: a spend is never taken back. Not read on a void, which is the take-back of a
 *    spend the customer was refused, never one told: a void is always held to the void cap and to the journal's.
 *  - `Confirmed`: an answer's `spends`, the entries the server recorded.
 *  - `Figures`: the figures a `use` answer gave, by code.
 *  - `Activated`: an activation, saved: another seat's journal is dropped, the same seat's is kept.
 *  - `Deactivated`: the journal goes with the rest of the licence.
 */
struct JournalOp {
  enum class Kind { Spend, Confirmed, Figures, Activated, Deactivated };

  Kind kind = Kind::Deactivated;
  /// The activation it is about (every kind but `Deactivated`).
  std::string about;

  // A spend.
  SpendKind spend = SpendKind::Offline;
  std::string id;
  std::string meter;
  std::int64_t n = 0;
  /// When (the guarded clock) the spend was made.
  std::int64_t at = 0;
  /// The most the meter's unsynced spends may come to with it.
  std::optional<std::int64_t> limit;
  /// Whether the customer was told it was made while only memory could hold it.
  bool told = false;

  // Confirmed.
  std::vector<Confirmed> entries;

  // Figures.
  std::map<std::string, MeterFigure> figures;

  static JournalOp spendOf(SpendKind kind, std::string about, std::string id, std::string meter, std::int64_t n,
                           std::int64_t at, std::optional<std::int64_t> limit = std::nullopt, bool told = false);
  static JournalOp confirmedOf(std::string about, std::vector<Confirmed> entries);
  static JournalOp figuresOf(std::string about, std::map<std::string, MeterFigure> figures);
  static JournalOp activatedOf(std::string about);
  static JournalOp deactivated();

  bool isSpend() const { return kind == Kind::Spend; }
  bool isVoid() const { return kind == Kind::Spend && spend == SpendKind::Void; }
  bool operator==(const JournalOp& other) const;
  bool operator!=(const JournalOp& other) const { return !(*this == other); }
};

/** The fields after an op; `refused` is true when it was a spend the full journal could not take, or whose limit forbids. */
struct Journaled {
  std::vector<SpendEntry> spends;
  std::map<std::string, MeterFigure> meterFigures;
  bool refused = false;
};

/**
 * The journal and the figures after `op`, landed on `latest` at `now` (the guarded clock). An op about another activation
 * than the one stored, or about none, changes nothing; a deactivation clears both, and an activation of another seat
 * does (the same seat activated again keeps them). Every op that lands drops the entries opened more than kPendingMaxMs
 * before `now`, unreported. An append the journal has no room for is `refused`.
 */
Journaled journaled(const StoredState& latest, const JournalOp& op, std::int64_t now);

/**
 * `ops` with neighbours folded together where landing them one after the other lands what landing the fold does: offline
 * spends of one meter (their counts added, the first's id and time kept), figures (the newer of each meter) and
 * confirmations (each id once, the larger count). A disk that keeps refusing the journal then costs the process one change
 * for a run of spends of one meter, not one for each; spends of meters that take turns are not folded across one another.
 */
std::vector<JournalOp> compacted(const std::vector<JournalOp>& ops);

/// `ops` with all but the newest kMaxPendingVoids void ops dropped: a process that cannot save keeps no more voids than the journal would.
std::vector<JournalOp> boundedVoids(const std::vector<JournalOp>& ops);

/** What landing ops on a state came to. */
struct LandedJournal {
  /// The state after every op: an empty journal, or no figures, is the field absent.
  StoredState state;
  /// Whether each op was refused.
  std::vector<bool> refused;
};

/// `ops` landed in order on `state` at `now`, and whether each was refused; the state itself when there are none.
LandedJournal landJournal(const StoredState& state, const std::vector<JournalOp>& ops, std::int64_t now);

}  // namespace keygrant::detail
