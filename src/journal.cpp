#include "journal.hpp"

#include <algorithm>
#include <unordered_map>

namespace keygrant::detail {

JournalOp JournalOp::spendOf(SpendKind kind, std::string about, std::string id, std::string meter, std::int64_t n,
                             std::int64_t at, std::optional<std::int64_t> limit, bool told) {
  JournalOp op;
  op.kind = Kind::Spend;
  op.spend = kind;
  op.about = std::move(about);
  op.id = std::move(id);
  op.meter = std::move(meter);
  op.n = n;
  op.at = at;
  op.limit = limit;
  op.told = told;
  return op;
}

JournalOp JournalOp::confirmedOf(std::string about, std::vector<Confirmed> entries) {
  JournalOp op;
  op.kind = Kind::Confirmed;
  op.about = std::move(about);
  op.entries = std::move(entries);
  return op;
}

JournalOp JournalOp::figuresOf(std::string about, std::map<std::string, MeterFigure> figures) {
  JournalOp op;
  op.kind = Kind::Figures;
  op.about = std::move(about);
  op.figures = std::move(figures);
  return op;
}

JournalOp JournalOp::activatedOf(std::string about) {
  JournalOp op;
  op.kind = Kind::Activated;
  op.about = std::move(about);
  return op;
}

JournalOp JournalOp::deactivated() { return JournalOp{}; }

bool JournalOp::operator==(const JournalOp& other) const {
  return kind == other.kind && about == other.about && spend == other.spend && id == other.id && meter == other.meter &&
         n == other.n && at == other.at && limit == other.limit && told == other.told && entries == other.entries &&
         figures == other.figures;
}

namespace {

/** Whether an answer's confirmation names a stored entry as recorded. A void is named by `void`, any other by how many the server holds. */
bool confirms(const SpendEntry& entry, const std::vector<Confirmed>& confirmed) {
  return std::any_of(confirmed.begin(), confirmed.end(), [&](const Confirmed& c) {
    if (c.id != entry.id) return false;
    return c.voided ? entry.kind == SpendKind::Void : entry.kind != SpendKind::Void && entry.n <= c.n;
  });
}

/** `stored` with the incoming figures, for each meter the newer by `at` kept (the stored one on a tie). */
std::map<std::string, MeterFigure> newest(std::map<std::string, MeterFigure> stored,
                                          const std::map<std::string, MeterFigure>& incoming) {
  for (const auto& [code, figure] : incoming) {
    const auto held = stored.find(code);
    if (held == stored.end() || figure.at > held->second.at) stored.insert_or_assign(code, figure);
  }
  return stored;
}

/** Whether a spend was told to the customer: so it lands whatever the limit and the cap say. A void never is. */
bool toldOf(const JournalOp& op) { return op.spend != SpendKind::Void && op.told; }

/** The index of the void a new void takes the place of: the oldest, when the journal already holds kMaxPendingVoids. */
std::optional<std::size_t> voidToReplace(const std::vector<SpendEntry>& journal) {
  std::optional<std::size_t> oldest;
  std::size_t voids = 0;
  for (std::size_t i = 0; i < journal.size(); ++i) {
    if (journal[i].kind != SpendKind::Void) continue;
    if (!oldest) oldest = i;
    ++voids;
  }
  return voids >= kMaxPendingVoids ? oldest : std::nullopt;
}

struct Appended {
  std::vector<SpendEntry> spends;
  bool refused = false;
};

/**
 * The journal an entry is opened in: for a void, without the oldest void when kMaxPendingVoids are held (the new one
 * takes its place), so the attempts of a device out of reach never fill the journal the spends it can make need. Whether
 * it has room: a new entry past the cap is refused, unless it was told.
 */
Appended opened(std::vector<SpendEntry> journal, const JournalOp& op) {
  const std::optional<std::size_t> replaced = op.spend == SpendKind::Void ? voidToReplace(journal) : std::nullopt;
  const std::size_t room = journal.size() - (replaced ? 1 : 0);
  if (room >= kMaxPendingSpends && !toldOf(op)) return Appended{std::move(journal), true};
  if (replaced) journal.erase(journal.begin() + static_cast<std::ptrdiff_t>(*replaced));
  journal.push_back(SpendEntry{op.id, op.meter, op.n, op.at, op.spend});
  return Appended{std::move(journal), false};
}

/**
 * The journal with an append applied: an `Offline` entry grows or is opened, the others are opened once by id; none that
 * would take the meter's unsynced spends past the op's limit (a void is no spend, and a told one lands whatever it says),
 * nor a new entry past the cap (opened). An offline spend that joins an entry loses its id, which is why no request
 * reports a spend still held in memory (Engine::reportable).
 */
Appended appended(std::vector<SpendEntry> journal, const JournalOp& op) {
  // A spend whose id the journal holds is the one it is: left as it is, whatever the limit now says.
  if (op.spend != SpendKind::Offline &&
      std::any_of(journal.begin(), journal.end(), [&](const SpendEntry& e) { return e.id == op.id; })) {
    return Appended{std::move(journal), false};
  }
  const bool limited = op.spend != SpendKind::Void && !toldOf(op) && op.limit.has_value();
  if (limited && saturatingSum(unsyncedOf(journal, op.meter), op.n) > *op.limit) return Appended{std::move(journal), true};
  if (op.spend == SpendKind::Offline) {
    const auto open = std::find_if(journal.begin(), journal.end(), [&](const SpendEntry& e) {
      return e.kind == SpendKind::Offline && e.meter == op.meter;
    });
    if (open != journal.end()) {
      open->n = saturatingSum(open->n, op.n);
      return Appended{std::move(journal), false};
    }
  }
  return opened(std::move(journal), op);
}

}  // namespace

Journaled journaled(const StoredState& latest, const JournalOp& op, std::int64_t now) {
  if (op.kind == JournalOp::Kind::Deactivated) return Journaled{};
  if (!latest.activationId || *latest.activationId != op.about) {
    if (op.kind == JournalOp::Kind::Activated) return Journaled{};
    return Journaled{latest.spends, latest.meterFigures, false};
  }
  std::vector<SpendEntry> live;
  for (const SpendEntry& e : latest.spends) {
    if (now - e.at <= kPendingMaxMs) live.push_back(e);
  }
  switch (op.kind) {
    case JournalOp::Kind::Activated: return Journaled{std::move(live), latest.meterFigures, false};
    case JournalOp::Kind::Confirmed: {
      std::vector<SpendEntry> kept;
      for (SpendEntry& e : live) {
        if (!confirms(e, op.entries)) kept.push_back(std::move(e));
      }
      return Journaled{std::move(kept), latest.meterFigures, false};
    }
    case JournalOp::Kind::Figures:
      return Journaled{std::move(live), newest(latest.meterFigures, op.figures), false};
    case JournalOp::Kind::Spend: {
      Appended landed = appended(std::move(live), op);
      return Journaled{std::move(landed.spends), latest.meterFigures, landed.refused};
    }
    case JournalOp::Kind::Deactivated: break;
  }
  return Journaled{};
}

namespace {

/** Confirmations with each id once as a count and once as a void, the larger count kept: the entries any of them would have dropped. */
std::vector<Confirmed> foldedConfirmations(const std::vector<Confirmed>& entries) {
  std::vector<Confirmed> folded;
  std::unordered_map<std::string, std::size_t> at;  // where each (kind, id) first stands in `folded`
  for (const Confirmed& entry : entries) {
    const auto held = at.find((entry.voided ? "v" : "c") + entry.id);
    if (held == at.end()) {
      at.emplace((entry.voided ? "v" : "c") + entry.id, folded.size());
      folded.push_back(entry);
    } else if (!entry.voided && entry.n > folded[held->second].n) {
      folded[held->second].n = entry.n;
    }
  }
  return folded;
}

}  // namespace

std::vector<JournalOp> compacted(const std::vector<JournalOp>& ops) {
  std::vector<JournalOp> out;
  for (const JournalOp& op : ops) {
    if (!out.empty()) {
      JournalOp& last = out.back();
      if (last.kind == JournalOp::Kind::Spend && op.kind == JournalOp::Kind::Spend && last.spend == SpendKind::Offline &&
          op.spend == SpendKind::Offline && last.about == op.about && last.meter == op.meter) {
        last.n = saturatingSum(last.n, op.n);
        continue;
      }
      if (last.kind == JournalOp::Kind::Figures && op.kind == JournalOp::Kind::Figures && last.about == op.about) {
        last.figures = newest(last.figures, op.figures);
        continue;
      }
      if (last.kind == JournalOp::Kind::Confirmed && op.kind == JournalOp::Kind::Confirmed && last.about == op.about) {
        std::vector<Confirmed> both = last.entries;
        both.insert(both.end(), op.entries.begin(), op.entries.end());
        last.entries = foldedConfirmations(both);
        continue;
      }
    }
    out.push_back(op);
  }
  return out;
}

std::vector<JournalOp> boundedVoids(const std::vector<JournalOp>& ops) {
  const std::size_t voids = static_cast<std::size_t>(std::count_if(ops.begin(), ops.end(), [](const JournalOp& op) { return op.isVoid(); }));
  std::size_t surplus = voids > kMaxPendingVoids ? voids - kMaxPendingVoids : 0;
  std::vector<JournalOp> out;
  for (const JournalOp& op : ops) {
    if (surplus > 0 && op.isVoid()) {
      --surplus;
      continue;
    }
    out.push_back(op);
  }
  return out;
}

LandedJournal landJournal(const StoredState& state, const std::vector<JournalOp>& ops, std::int64_t now) {
  LandedJournal out;
  out.state = state;
  for (const JournalOp& op : ops) {
    Journaled landed = journaled(out.state, op, now);
    out.refused.push_back(landed.refused);
    out.state.spends = std::move(landed.spends);
    out.state.meterFigures = std::move(landed.meterFigures);
  }
  return out;
}

}  // namespace keygrant::detail
