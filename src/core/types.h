// World state: cars, players, race, career progress. Plain data, no engine types.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "bitstream.h"
#include "common.h"
#include "rng.h"

namespace nfc {

// --- car state on the wire -------------------------------------------------
enum CarFlag : u8 {
    kCarBraking = 1u << 0,
    kCarReverse = 1u << 1,
    kCarNos = 1u << 2,
    kCarDrift = 1u << 3,
    kCarHorn = 1u << 4,
    kCarHidden = 1u << 5,
    kCarLightsBroken = 1u << 6,
    kCarEngineDead = 1u << 7,
};

struct CarState {
    Vec3 pos;                     // meters, game world space
    Vec3 vel;                     // m/s
    f32 yaw = 0, pitch = 0, roll = 0;  // radians
    f32 steer = 0;                // -1..1
    u16 gear = 1;
    u8 model = 0;                 // car model index used by the adapter
    u8 flags = 0;

    bool sane() const {
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return false;
        if (!std::isfinite(vel.x) || !std::isfinite(vel.y) || !std::isfinite(vel.z)) return false;
        if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll)) return false;
        if (length2(pos) > 4.0e8f) return false;  // > 20 km from the origin: garbage
        return true;
    }

    void encode(BitWriter& w) const {
        w.qPos(pos.x);
        w.qPos(pos.y);
        w.qPos(pos.z);
        w.qVel(vel.x);
        w.qVel(vel.y);
        w.qVel(vel.z);
        w.qAngle16(yaw);
        w.qAngleS(pitch);
        w.qAngleS(roll);
        w.i8v(static_cast<i8>(clampi(static_cast<i32>(std::lround(steer * 127.0f)), -127, 127)));
        w.u16v(gear);
        w.u8v(model);
        w.u8v(flags);
    }

    bool decode(BitReader& r) {
        pos.x = r.qPos();
        pos.y = r.qPos();
        pos.z = r.qPos();
        vel.x = r.qVel();
        vel.y = r.qVel();
        vel.z = r.qVel();
        yaw = r.qAngle16();
        pitch = r.qAngleS();
        roll = r.qAngleS();
        steer = static_cast<f32>(r.i8v()) / 127.0f;
        gear = r.u16v();
        model = r.u8v();
        flags = r.u8v();
        return r.ok();
    }

    static constexpr u32 kWireSize = 4 * 3 + 2 * 3 + 2 + 2 + 2 + 1 + 2 + 1 + 1;
};

inline CarState lerpCar(const CarState& a, const CarState& b, f32 t) {
    CarState o = b;
    o.pos = lerp(a.pos, b.pos, t);
    o.vel = lerp(a.vel, b.vel, t);
    o.yaw = a.yaw + wrapPi(b.yaw - a.yaw) * t;
    o.pitch = a.pitch + (b.pitch - a.pitch) * t;
    o.roll = a.roll + (b.roll - a.roll) * t;
    o.steer = a.steer + (b.steer - a.steer) * t;
    return o;
}

// --- entities --------------------------------------------------------------
enum class EKind : u8 { Player = 0, Traffic = 1, Bot = 2 };

const char* toString(EKind k);

struct Entity {
    EntityId id = kNoEntity;
    EKind kind = EKind::Traffic;
    u8 model = 0;
    CarState st;
    CarState prev;
    u32 updatedAtMs = 0;
    u32 prevAtMs = 0;
    PlayerId owner = kNoPlayer;   // mirror of the ownership decision, for display
    u32 epoch = 0;
    u32 gameHandle = 0;           // adapter-side handle; never sent
    bool active = false;
    bool spawnSent = false;
    u16 checkpoint = 0;

    void observe(const CarState& s, u32 nowMs) {
        prev = st;
        prevAtMs = updatedAtMs;
        st = s;
        updatedAtMs = nowMs;
    }

    CarState interpolate(u32 nowMs, u32 interpDelayMs) const {
        if (prevAtMs == 0 || updatedAtMs <= prevAtMs) return st;
        i32 target = static_cast<i32>(nowMs) - static_cast<i32>(interpDelayMs);
        f32 t = static_cast<f32>(target - static_cast<i32>(prevAtMs)) /
                static_cast<f32>(static_cast<i32>(updatedAtMs) - static_cast<i32>(prevAtMs));
        t = clampf(t, 0.0f, 1.0f);
        return lerpCar(prev, st, t);
    }
};

// --- players ---------------------------------------------------------------
struct PlayerInfo {
    PlayerId slot = kNoPlayer;
    std::string name = "player";
    bool connected = false;
    bool ready = false;     // ready for the next race
    bool loaded = false;    // the adapter reports an active car
    u32 lastHeardMs = 0;
    u32 rttMs = 0;
    u32 zoneId = 0;
    u32 model = 0;   // car model id received from the owner
    u32 paint = 0;   // 0x00RRGGBB, 0 = unknown
    EntityId carId = kNoEntity;
    u8 place = 0;
    u16 checkpoint = 0;
    u16 lap = 0;
    u32 raceTimeMs = 0;
    bool finished = false;
    u32 finishMs = 0;

    void encode(BitWriter& w) const {
        w.u8v(slot);
        w.u8v(static_cast<u8>((connected ? 1 : 0) | (ready ? 2 : 0) | (loaded ? 4 : 0) | (finished ? 8 : 0)));
        w.u16v(static_cast<u16>(std::min<u32>(rttMs, 65535)));
        w.u32v(zoneId);
        w.u32v(model);
        w.u32v(paint);
        w.u32v(carId);
        w.u8v(place);
        w.str(name);
    }
    bool decode(BitReader& r) {
        slot = r.u8v();
        u8 f = r.u8v();
        connected = (f & 1) != 0;
        ready = (f & 2) != 0;
        loaded = (f & 4) != 0;
        finished = (f & 8) != 0;
        rttMs = r.u16v();
        zoneId = r.u32v();
        model = r.u32v();
        paint = r.u32v();
        carId = r.u32v();
        place = r.u8v();
        name = r.str();
        return r.ok();
    }
};

// --- race ------------------------------------------------------------------
enum class RacePhase : u8 { Idle = 0, Lobby = 1, Countdown = 2, Racing = 3, Finished = 4, Aborted = 5 };
const char* toString(RacePhase p);

struct RacePlayer {
    bool joined = false;
    bool ready = false;
    bool finished = false;
    bool dnf = false;
    u16 checkpoint = 0;
    u16 lap = 0;
    u32 timeMs = 0;
    u32 finishMs = 0;
    u8 place = 0;
};

struct RaceState {
    RacePhase phase = RacePhase::Idle;
    u16 eventId = 0;
    u32 routeId = 0;
    u32 seed = 0;
    u16 checkpointCount = 0;
    u32 openAtMs = 0;
    u32 startAtMs = 0;
    u32 resultsAtMs = 0;
    u8 finisherCount = 0;
    u8 version = 0;  // bumped by the host on every change
    RacePlayer p[kMaxPlayers];

    bool participant(PlayerId s) const { return s < kMaxPlayers && p[s].joined; }
    bool allDone() const;
    void reset();

    void encode(BitWriter& w) const;
    bool decode(BitReader& r);
    u64 digest() const;
};

// --- career progression ----------------------------------------------------
// A shared career: one cash pool, one reputation, one set of completed events.
// Every event is an idempotent key; the effect of a key is a pure function, so
// the host sends the key and every machine derives the same numbers.
enum CareerType : u32 {
    kCareerRace = 1,
    kCareerOutrun = 2,
    kCareerSponsor = 3,
    kCareerMagazine = 4,
    kCareerDvd = 5,
    kCareerUnique = 6,
};

u32 careerKey(u32 type, u32 id);

struct CareerEffect {
    u32 cashDelta = 0;
    u16 repDelta = 0;
    u16 unlockCar = 0xFFFF;
};

CareerEffect careerEffect(u32 key);
const char* careerEventName(u32 key);

struct ProgressLedger {
    u32 revision = 0;
    std::vector<u32> order;
    std::set<u32> keys;
    u32 cash = 0;
    u32 rep = 0;

    bool has(u32 key) const { return keys.count(key) != 0; }
    bool apply(u32 key);  // false if it was already there (nothing changed)
    void reset() {
        revision = 0;
        order.clear();
        keys.clear();
        cash = 0;
        rep = 0;
    }
    void encode(BitWriter& w) const;
    bool decode(BitReader& r);
    u64 digest() const;
};

// --- world -----------------------------------------------------------------
class World {
public:
    u32 sessionId = 0;
    u32 sessionSeed = 0;
    u32 hostMs = 0;
    PlayerId localSlot = kNoPlayer;
    std::string hostName;
    std::string sessionName;

    PlayerInfo players[kMaxPlayers];
    std::map<EntityId, Entity> entities;
    RaceState race;
    ProgressLedger progress;

    World() { reset(); }
    void reset();

    Entity& upsert(EntityId id, EKind kind);
    Entity* find(EntityId id);
    const Entity* find(EntityId id) const;
    void remove(EntityId id);
    void clearEntities();
    std::size_t activeCount(EKind kind) const;

    PlayerInfo* bySlot(PlayerId slot);
    const PlayerInfo* bySlot(PlayerId slot) const;
    int connectedCount() const;

    // Canonical digests: same quantized fields on every machine => same number.
    u64 digestEntities() const;
    u64 digestRace() const { return race.digest(); }
    u64 digestProgress() const { return progress.digest(); }
    u64 digestAll() const;

    std::string playerList() const;
};

// Host-side helpers that mutate World::race with the rules in docs/SYNC-RULES.md.
bool raceOpen(World& w, u16 eventId, u32 routeId, u16 checkpointCount, u32 nowMs);
bool raceSetReady(World& w, PlayerId slot, bool ready, u16 eventId);
bool raceBeginCountdown(World& w, u32 nowMs, u32 countdownMs = kCountdownMs);
void raceTick(World& w, u32 nowMs);
bool raceSetProgress(World& w, PlayerId slot, u16 checkpoint, u16 lap, u32 timeMs);
bool raceSetFinished(World& w, PlayerId slot, u32 timeMs);
bool raceAbort(World& w, u32 nowMs);
void raceApplyResults(World& w, u32 nowMs);

}  // namespace nfc
