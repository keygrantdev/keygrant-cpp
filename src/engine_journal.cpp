// The stored state under the engine and the journal of unsynced spends beside it: what this process could not save is kept
// in memory, counts everywhere the stored state is read, and is saved with the next save that can, as a trial the disk
// refused is kept (trials.cpp). A process that ends first loses the report, never the customer's use.
#include <algorithm>

#include "engine.hpp"

namespace keygrant::detail {

namespace {

/** A spend as it is kept in memory: marked told. A void is no spend told. */
JournalOp told(const JournalOp& op) {
  JournalOp kept = op;
  if (kept.isSpend() && kept.spend != SpendKind::Void) kept.told = true;
  return kept;
}

}  // namespace

std::vector<JournalOp> Engine::heldChanges() {
  std::lock_guard<std::mutex> lock(heldMutex_);
  return held_.ops;
}

/** The stored state, as the storage has it. */
StoredState Engine::stored(bool readOnly) { return a_.storage->read(readOnly).value_or(StoredState{}); }

/**
 * The stored state with the journal changes this process could not save landed on it, as they will be; `readOnly` behind
 * a call that has not finished, so the read writes nothing either.
 */
View Engine::view(bool readOnly) {
  StoredState state = stored(readOnly);
  const std::vector<JournalOp> held = heldChanges();
  if (held.empty()) return View{std::move(state), false};
  StoredState landed = landJournal(state, held, guardedNow(clockNow(), state.lastSeen)).state;
  const bool carried = !landed.spends.empty();
  return View{std::move(landed), carried};
}

StoredState Engine::load(bool readOnly) { return view(readOnly).state; }

std::optional<StoredState> Engine::seenState() {
  try {
    return load(true);
  } catch (...) {
    return std::nullopt;
  }
}

/**
 * The journal a request reports for `view` (the state a call read): the journal as stored, with what this process could
 * not save that opens an entry of its own, once, by its id: an `Unanswered` spend and a `Void`. A spend held in memory
 * that was made offline is withheld: it joins the entry of another when it lands, so a report of it under its own id would
 * be counted a second time when the entry it joined is reported. It is reported once it has landed, which the next call
 * that can save does. The entries the server has since confirmed are left out: there is no need to say again what it holds.
 * A view that carried held spends is judged the same, though they may have landed since.
 */
std::vector<SpendEntry> Engine::reportable(const View& view) {
  const std::vector<JournalOp> held = heldChanges();
  if (!view.carried && held.empty()) return view.state.spends;
  StoredState current;
  try {
    current = stored(true);
  } catch (...) {
    return {};
  }
  if (current.activationId != view.state.activationId) return {};
  std::vector<JournalOp> reported;
  for (const JournalOp& op : held) {
    if ((op.isSpend() && op.spend != SpendKind::Offline) || op.kind == JournalOp::Kind::Confirmed) reported.push_back(op);
  }
  return landJournal(current, reported, guardedNow(clockNow(), current.lastSeen)).state.spends;
}

/**
 * Persist a patch onto the state as it is stored NOW, by what it is based on, advancing the clock guard unless it is being
 * brought back on the server's evidence (`guard`).
 */
void Engine::save(const StatePatch& patch, std::optional<std::int64_t> guard, const Basis& basis) {
  saveWith(patch, guard, basis, {});
}

/**
 * `save` with changes to the journal (`ops`), landed on the state as stored now, after the ones this process could not
 * save before. Whether each of `ops` was refused, the journal being full or its limit forbidding it. Throws when the save
 * fails; the changes stay held until one lands. Through the storage's update() when it has one, which merges again onto a
 * state another process saved meanwhile.
 */
std::vector<bool> Engine::saveWith(const StatePatch& patch, std::optional<std::int64_t> guard, const Basis& basis,
                                   const std::vector<JournalOp>& ops) {
  std::size_t before = 0;
  std::vector<JournalOp> all;
  {
    std::lock_guard<std::mutex> lock(heldMutex_);
    before = held_.ops.size();
    all = held_.ops;
    all.insert(all.end(), ops.begin(), ops.end());
    ++held_.saving;
  }
  std::vector<bool> refused;
  try {
    const std::function<StoredState(const StoredState&)> merge = [&](const StoredState& latest) {
      const std::int64_t now = clockNow();
      LandedJournal landed = landJournal(latest, all, guardedNow(now, latest.lastSeen));
      // The last merge is the one that landed, when the storage ran it again on a newer state.
      refused = std::move(landed.refused);
      StoredState next = merged(landed.state, patch, basis);
      next.lastSeen = savedLastSeen(latest.lastSeen, now, guard);
      return next;
    };
    if (!a_.storage->update(merge)) a_.storage->write(merge(stored(false)));
  } catch (...) {
    std::lock_guard<std::mutex> lock(heldMutex_);
    --held_.saving;
    throw;
  }
  {
    std::lock_guard<std::mutex> lock(heldMutex_);
    --held_.saving;
    // Those saved are the first `before`; others may have been kept meanwhile, beside them.
    held_.ops.erase(held_.ops.begin(),
                    held_.ops.begin() + static_cast<std::ptrdiff_t>(std::min(before, held_.ops.size())));
  }
  return std::vector<bool>(refused.begin() + static_cast<std::ptrdiff_t>(std::min(before, refused.size())),
                           refused.end());
}

/**
 * `saveWith`, whose failure fails nothing: the changes it could not save are kept for this process, those the journal had
 * room for (judged on `view`, the state the call read, when the save failed). Whether each of `ops` was refused.
 */
std::vector<bool> Engine::saveOps(const StoredState& view, const StatePatch& patch, const Basis& basis,
                                  const std::vector<JournalOp>& ops) {
  try {
    return saveWith(patch, std::nullopt, basis, ops);
  } catch (...) {
    return keepUnsaved(view, ops);
  }
}

/** Keep `ops` for this process, those the journal has room for on `view`. Whether each was refused. */
std::vector<bool> Engine::keepUnsaved(const StoredState& view, const std::vector<JournalOp>& ops) {
  const std::vector<bool> refused = landJournal(view, ops, guardedNow(clockNow(), view.lastSeen)).refused;
  std::lock_guard<std::mutex> lock(heldMutex_);
  // A spend is told to the customer from here on: it lands whatever the journal holds by then. A void is no spend told,
  // so it is held to the journal's rules, and to the newest kMaxPendingVoids here.
  for (std::size_t i = 0; i < ops.size(); ++i) {
    if (!refused[i]) held_.ops.push_back(told(ops[i]));
  }
  if (held_.saving == 0) held_.ops = boundedVoids(compacted(held_.ops));
  return refused;
}

/** Save what this process could not, best-effort, at the start of a call that may write. */
void Engine::saveUnsavedJournal() {
  if (!heldChanges().empty()) saveBestEffort(StatePatch{});
}

}  // namespace keygrant::detail
