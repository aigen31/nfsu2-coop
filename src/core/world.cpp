#include "types.h"

#include <cstdio>

#include "log.h"

namespace nfc {

const char* toString(EKind k) {
    switch (k) {
        case EKind::Player: return "player";
        case EKind::Traffic: return "traffic";
        case EKind::Bot: return "bot";
    }
    return "?";
}

const char* toString(RacePhase p) {
    switch (p) {
        case RacePhase::Idle: return "idle";
        case RacePhase::Lobby: return "lobby";
        case RacePhase::Countdown: return "countdown";
        case RacePhase::Racing: return "racing";
        case RacePhase::Finished: return "finished";
        case RacePhase::Aborted: return "aborted";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Career effects. A key is the only thing on the wire; these numbers are the
// same on every machine, so two clients cannot disagree about the reward.
// key = (type << 16) | id
// ---------------------------------------------------------------------------
u32 careerKey(u32 type, u32 id) { return (type << 16) | (id & 0xFFFF); }

CareerEffect careerEffect(u32 key) {
    CareerEffect e{};
    const u32 type = (key >> 16) & 0xFF;
    const u32 id = key & 0xFFFF;
    switch (type) {
        case kCareerRace:
            e.cashDelta = 500 + 100 * (id % 5);
            e.repDelta = static_cast<u16>(10 + (id % 3) * 5);
            break;
        case kCareerOutrun:
            e.cashDelta = 200 + 25 * (id % 4);
            e.repDelta = 5;
            break;
        case kCareerSponsor:
            e.cashDelta = 1500;
            e.repDelta = 25;
            break;
        case kCareerMagazine:
            e.cashDelta = 0;
            e.repDelta = 40;
            break;
        case kCareerDvd:
            e.cashDelta = 500;
            e.repDelta = 15;
            break;
        case kCareerUnique:
            e.cashDelta = 0;
            e.repDelta = 0;
            e.unlockCar = static_cast<u16>(id);
            break;
        default:
            break;
    }
    return e;
}

const char* careerEventName(u32 key) {
    static thread_local char buf[64];
    const u32 type = (key >> 16) & 0xFF;
    const u32 id = key & 0xFFFF;
    const char* prefix = "event";
    switch (type) {
        case kCareerRace: prefix = "race"; break;
        case kCareerOutrun: prefix = "outrun"; break;
        case kCareerSponsor: prefix = "sponsor"; break;
        case kCareerMagazine: prefix = "magazine"; break;
        case kCareerDvd: prefix = "dvd"; break;
        case kCareerUnique: prefix = "unique"; break;
        default: break;
    }
    std::snprintf(buf, sizeof(buf), "%s #%u", prefix, id);
    return buf;
}

// ---------------------------------------------------------------------------
// ProgressLedger
// ---------------------------------------------------------------------------
bool ProgressLedger::apply(u32 key) {
    if (key == 0) return false;
    if (!keys.insert(key).second) return false;
    const CareerEffect e = careerEffect(key);
    cash += e.cashDelta;
    rep += e.repDelta;
    order.push_back(key);
    ++revision;
    return true;
}

void ProgressLedger::encode(BitWriter& w) const {
    w.u32v(revision);
    w.u32v(cash);
    w.u32v(rep);
    w.u16v(static_cast<u16>(std::min<std::size_t>(order.size(), 0xFFFF)));
    for (u32 k : order) w.u32v(k);
}

bool ProgressLedger::decode(BitReader& r) {
    ProgressLedger tmp;
    tmp.revision = r.u32v();
    tmp.cash = r.u32v();
    tmp.rep = r.u32v();
    const u32 n = r.u16v();
    for (u32 i = 0; i < n && r.ok(); ++i) {
        const u32 k = r.u32v();
        if (tmp.keys.insert(k).second) {
            tmp.order.push_back(k);
            const CareerEffect e = careerEffect(k);
            // cash/rep were sent explicitly; keep the sent values, not a re-sum
            (void)e;
        }
    }
    if (!r.ok()) return false;
    *this = std::move(tmp);
    return true;
}

u64 ProgressLedger::digest() const {
    BitWriter w;
    encode(w);
    return fnv1a64(w.data(), w.size());
}

// ---------------------------------------------------------------------------
// RaceState
// ---------------------------------------------------------------------------
void RaceState::reset() {
    *this = RaceState{};
}

bool RaceState::allDone() const {
    int joined = 0;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (!p[i].joined) continue;
        ++joined;
        if (!p[i].finished && !p[i].dnf) return false;
    }
    return joined > 0;
}

void RaceState::encode(BitWriter& w) const {
    w.u8v(static_cast<u8>(phase));
    w.u16v(eventId);
    w.u32v(routeId);
    w.u32v(seed);
    w.u16v(checkpointCount);
    w.u32v(openAtMs);
    w.u32v(startAtMs);
    w.u32v(resultsAtMs);
    w.u8v(finisherCount);
    w.u8v(version);
    for (int i = 0; i < kMaxPlayers; ++i) {
        const RacePlayer& rp = p[i];
        u8 f = 0;
        if (rp.joined) f |= 1;
        if (rp.ready) f |= 2;
        if (rp.finished) f |= 4;
        if (rp.dnf) f |= 8;
        w.u8v(f);
        w.u16v(rp.checkpoint);
        w.u16v(rp.lap);
        w.u32v(rp.timeMs);
        w.u32v(rp.finishMs);
        w.u8v(rp.place);
    }
}

bool RaceState::decode(BitReader& r) {
    RaceState tmp;
    tmp.phase = static_cast<RacePhase>(r.u8v());
    tmp.eventId = r.u16v();
    tmp.routeId = r.u32v();
    tmp.seed = r.u32v();
    tmp.checkpointCount = r.u16v();
    tmp.openAtMs = r.u32v();
    tmp.startAtMs = r.u32v();
    tmp.resultsAtMs = r.u32v();
    tmp.finisherCount = r.u8v();
    tmp.version = r.u8v();
    for (int i = 0; i < kMaxPlayers; ++i) {
        RacePlayer& rp = tmp.p[i];
        const u8 f = r.u8v();
        rp.joined = (f & 1) != 0;
        rp.ready = (f & 2) != 0;
        rp.finished = (f & 4) != 0;
        rp.dnf = (f & 8) != 0;
        rp.checkpoint = r.u16v();
        rp.lap = r.u16v();
        rp.timeMs = r.u32v();
        rp.finishMs = r.u32v();
        rp.place = r.u8v();
    }
    if (!r.ok()) return false;
    if (static_cast<u8>(tmp.phase) > static_cast<u8>(RacePhase::Aborted)) return false;
    *this = tmp;
    return true;
}

u64 RaceState::digest() const {
    BitWriter w;
    encode(w);
    return fnv1a64(w.data(), w.size());
}

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
void World::reset() {
    sessionId = 0;
    sessionSeed = 0;
    hostMs = 0;
    localSlot = kNoPlayer;
    hostName.clear();
    sessionName.clear();
    for (int i = 0; i < kMaxPlayers; ++i) {
        players[i] = PlayerInfo{};
        players[i].slot = static_cast<PlayerId>(i);
        players[i].name = "player " + std::to_string(i + 1);
    }
    entities.clear();
    race.reset();
    progress.reset();
}

Entity& World::upsert(EntityId id, EKind kind) {
    auto it = entities.find(id);
    if (it != entities.end()) return it->second;
    Entity e;
    e.id = id;
    e.kind = kind;
    e.active = true;
    auto res = entities.emplace(id, e);
    return res.first->second;
}

Entity* World::find(EntityId id) {
    auto it = entities.find(id);
    return it == entities.end() ? nullptr : &it->second;
}

const Entity* World::find(EntityId id) const {
    auto it = entities.find(id);
    return it == entities.end() ? nullptr : &it->second;
}

void World::remove(EntityId id) { entities.erase(id); }

void World::clearEntities() { entities.clear(); }

std::size_t World::activeCount(EKind kind) const {
    std::size_t n = 0;
    for (const auto& kv : entities)
        if (kv.second.active && kv.second.kind == kind) ++n;
    return n;
}

PlayerInfo* World::bySlot(PlayerId slot) {
    return slot < kMaxPlayers ? &players[slot] : nullptr;
}

const PlayerInfo* World::bySlot(PlayerId slot) const {
    return slot < kMaxPlayers ? &players[slot] : nullptr;
}

int World::connectedCount() const {
    int n = 0;
    for (const PlayerInfo& p : players)
        if (p.connected) ++n;
    return n;
}

u64 World::digestEntities() const {
    u64 h = 0xCBF29CE484222325ull;
    for (const auto& kv : entities) {
        const Entity& e = kv.second;
        if (!e.active) continue;
        BitWriter w(64);
        w.u32v(e.id);
        w.u8v(static_cast<u8>(e.kind));
        w.u8v(e.owner);
        w.u32v(e.epoch);
        e.st.encode(w);
        h ^= fnv1a64(w.data(), w.size(), h);
        h *= 0x100000001B3ull;
    }
    return h;
}

u64 World::digestAll() const {
    u64 h = digestEntities();
    h ^= digestRace() * 0x9E3779B97F4A7C15ull;
    h ^= digestProgress() * 0xC2B2AE3D27D4EB4Full;
    return h;
}

std::string World::playerList() const {
    std::string s;
    char line[160];
    for (const PlayerInfo& p : players) {
        if (!p.connected) continue;
        std::snprintf(line, sizeof(line), "%d. %s%s%s\n", static_cast<int>(p.slot) + 1, p.name.c_str(),
                      p.slot == 0 ? " (host)" : "", p.ready ? " [ready]" : "");
        s += line;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Race rules
// ---------------------------------------------------------------------------
namespace {
void bump(RaceState& r) { ++r.version; }

// Places come from the finish TIME, not from the order messages happened to
// arrive: on a lossy link the faster car's packet can be the later one.
void assignPlaces(RaceState& r) {
    // Recompute from scratch on every finish: a later packet can carry an
    // earlier time, and a stale "already placed" flag must not freeze a place.
    std::vector<int> order;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (r.p[i].joined && r.p[i].finished) {
            r.p[i].place = 0;
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (r.p[a].finishMs != r.p[b].finishMs) return r.p[a].finishMs < r.p[b].finishMs;
        return a < b;
    });
    u8 place = 1;
    for (int slot : order) r.p[slot].place = place++;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (!r.p[i].joined || r.p[i].finished) continue;
        r.p[i].place = 0;  // assigned when the race ends
    }
}

void placeDnf(RaceState& r) {
    assignPlaces(r);
    u8 place = 1;
    for (int i = 0; i < kMaxPlayers; ++i)
        if (r.p[i].joined && r.p[i].finished) ++place;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (!r.p[i].joined || r.p[i].finished) continue;
        r.p[i].dnf = true;
        r.p[i].place = place++;
    }
}
}  // namespace

bool raceOpen(World& w, u16 eventId, u32 routeId, u16 checkpointCount, u32 nowMs) {
    if (w.race.phase == RacePhase::Countdown || w.race.phase == RacePhase::Racing) {
        NFC_WARN("race: cannot open event %u while %s", eventId, toString(w.race.phase));
        return false;
    }
    RaceState fresh;
    fresh.phase = RacePhase::Lobby;
    fresh.eventId = eventId;
    fresh.routeId = routeId;
    fresh.checkpointCount = checkpointCount;
    fresh.seed = static_cast<u32>(mix64(w.sessionSeed, eventId));
    fresh.openAtMs = nowMs;
    fresh.version = w.race.phase == RacePhase::Idle ? 1 : static_cast<u8>(w.race.version + 1);
    for (int i = 0; i < kMaxPlayers; ++i) {
        // Only players who actually sit in a car race. A dedicated host, or a
        // player still in the menus, spectates instead of waiting forever.
        if (w.players[i].connected && w.players[i].loaded) {
            fresh.p[i].joined = true;
            fresh.p[i].ready = false;
        }
    }
    w.race = fresh;
    NFC_INFO("race: lobby open event=%u route=%u checkpoints=%u players=%d", eventId, routeId, checkpointCount,
             w.connectedCount());
    return true;
}

bool raceSetReady(World& w, PlayerId slot, bool ready, u16 eventId) {
    if (w.race.phase != RacePhase::Lobby) return false;
    if (slot >= kMaxPlayers || !w.race.p[slot].joined) return false;
    if (eventId != 0 && eventId != w.race.eventId) return false;
    if (w.race.p[slot].ready == ready) return false;
    w.race.p[slot].ready = ready;
    w.players[slot].ready = ready;
    bump(w.race);
    return true;
}

bool raceBeginCountdown(World& w, u32 nowMs, u32 countdownMs) {
    if (w.race.phase != RacePhase::Lobby) return false;
    int joined = 0, ready = 0;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (!w.race.p[i].joined) continue;
        ++joined;
        if (w.race.p[i].ready) ++ready;
    }
    if (joined == 0) return false;
    w.race.phase = RacePhase::Countdown;
    w.race.startAtMs = nowMs + countdownMs;
    bump(w.race);
    NFC_INFO("race: countdown event=%u joined=%d ready=%d startAt=%u", w.race.eventId, joined, ready,
             w.race.startAtMs);
    return true;
}

void raceTick(World& w, u32 nowMs) {
    RaceState& r = w.race;
    if (r.phase == RacePhase::Countdown && static_cast<i32>(nowMs - r.startAtMs) >= 0) {
        r.phase = RacePhase::Racing;
        for (int i = 0; i < kMaxPlayers; ++i) {
            if (r.p[i].joined) {
                r.p[i].timeMs = 0;
                r.p[i].checkpoint = 0;
                r.p[i].lap = 0;
            }
        }
        bump(r);
        NFC_INFO("race: GO event=%u", r.eventId);
        return;
    }
    if (r.phase == RacePhase::Racing) {
        // A player who disconnects mid-race is a DNF, never a winner.
        for (int i = 0; i < kMaxPlayers; ++i) {
            if (r.p[i].joined && !r.p[i].finished && !r.p[i].dnf && !w.players[i].connected) {
                r.p[i].dnf = true;
                bump(r);
                NFC_INFO("race: slot %d disconnected -> DNF", i);
            }
        }
        if (r.allDone()) {
            r.phase = RacePhase::Finished;
            r.resultsAtMs = nowMs;
            placeDnf(r);
            bump(r);
            NFC_INFO("race: finished event=%u", r.eventId);
        }
    }
}

bool raceSetProgress(World& w, PlayerId slot, u16 checkpoint, u16 lap, u32 timeMs) {
    RaceState& r = w.race;
    if (r.phase != RacePhase::Racing) return false;
    if (slot >= kMaxPlayers) return false;
    RacePlayer& rp = r.p[slot];
    if (!rp.joined || rp.finished || rp.dnf) return false;

    // Anti-cheat: a client may move one checkpoint forward at a time and never
    // past the end. A teleport (or a replayed old packet) is ignored.
    if (checkpoint > r.checkpointCount) return false;
    if (checkpoint > static_cast<u16>(rp.checkpoint + 1)) {
        NFC_WARN("race: slot %d skipped checkpoints %u -> %u, rejected", slot, rp.checkpoint, checkpoint);
        return false;
    }
    if (lap > static_cast<u16>(rp.lap + 1)) return false;
    if (lap < rp.lap) return false;
    if (checkpoint < rp.checkpoint) return false;

    bool changed = false;
    if (checkpoint != rp.checkpoint) {
        rp.checkpoint = checkpoint;
        changed = true;
    }
    if (lap != rp.lap) {
        rp.lap = lap;
        changed = true;
    }
    if (timeMs > rp.timeMs) {
        rp.timeMs = timeMs;
        changed = true;
    }
    w.players[slot].checkpoint = rp.checkpoint;
    w.players[slot].lap = rp.lap;
    w.players[slot].raceTimeMs = rp.timeMs;
    if (changed) bump(r);
    return changed;
}

bool raceSetFinished(World& w, PlayerId slot, u32 timeMs) {
    RaceState& r = w.race;
    if (r.phase != RacePhase::Racing) return false;
    if (slot >= kMaxPlayers) return false;
    RacePlayer& rp = r.p[slot];
    if (!rp.joined || rp.finished || rp.dnf) return false;
    if (timeMs == 0) return false;
    rp.finished = true;
    rp.finishMs = timeMs;
    rp.timeMs = timeMs;
    ++r.finisherCount;
    assignPlaces(r);
    w.players[slot].finished = true;
    w.players[slot].finishMs = timeMs;
    w.players[slot].place = rp.place;
    bump(r);
    NFC_INFO("race: slot %d finished place=%u time=%ums", slot, rp.place, timeMs);
    if (r.allDone()) {
        r.phase = RacePhase::Finished;
        r.resultsAtMs = w.hostMs;
        placeDnf(r);
        bump(r);
    }
    return true;
}

bool raceAbort(World& w, u32 nowMs) {
    RaceState& r = w.race;
    if (r.phase == RacePhase::Idle || r.phase == RacePhase::Finished) return false;
    r.phase = RacePhase::Aborted;
    r.resultsAtMs = nowMs;
    placeDnf(r);
    bump(r);
    return true;
}

void raceApplyResults(World& w, u32 nowMs) {
    RaceState& r = w.race;
    if (r.phase != RacePhase::Finished && r.phase != RacePhase::Aborted) return;
    if (r.eventId == 0) return;
    // Career progress: only a finished race (not an abort) counts, and only
    // once per event. The ledger makes that idempotent.
    if (r.phase == RacePhase::Finished && w.progress.apply(careerKey(1, r.eventId))) {
        const CareerEffect e = careerEffect(careerKey(1, r.eventId));
        NFC_INFO("career: %s completed, +%u cash +%u rep (rev %u)", careerEventName(careerKey(1, r.eventId)),
                 e.cashDelta, e.repDelta, w.progress.revision);
    }
    (void)nowMs;
}

}  // namespace nfc

