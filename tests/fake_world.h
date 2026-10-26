// A fake game world: plain data, a kinematic car and a handful of traffic cars.
// It implements the same IGameWorld the plugin implements over game memory, so
// the whole session layer runs and is tested without launching NFSU2.
#pragma once

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/session.h"

namespace nfc {

class FakeWorld : public IGameWorld {
public:
    // Local car
    CarState car;
    bool hasCar = true;
    u32 zone = 1;
    f32 speed = 400.0f;  // m/s, so a race takes well under a second

    // Race route: a straight line along +x with checkpoints every `cpSpacing`.
    u16 checkpointCount = 8;
    f32 cpSpacing = 20.0f;
    bool racing = false;
    bool finished = false;
    u16 lastCheckpoint = 0;
    u32 raceStartHostMs = 0;
    u32 lastProgressMs = 0;

    // Host-only traffic simulation
    bool simulateTraffic = false;
    int trafficCount = 0;
    struct Traffic {
        u32 handle = 0;
        u8 model = 0;
        f32 x = 0;
        f32 z = 0;
        f32 speed = 12.0f;
    };
    std::vector<Traffic> traffic;

    // Puppets (remote cars the session told us to show)
    struct Puppet {
        EKind kind = EKind::Traffic;
        u8 model = 0;
        CarState st;
        u32 updates = 0;
    };
    std::map<EntityId, Puppet> puppets;
    int spawns = 0;
    int despawns = 0;

    // Observations
    std::vector<std::string> notices;
    ProgressLedger ledger;
    RacePhase phase = RacePhase::Idle;
    int raceCallbacks = 0;
    bool connected = false;
    std::string connectionReason;

    // Hooks the harness wires into the session (like the plugin does)
    std::function<void(u16, u16, u32)> onProgress;
    std::function<void(u32)> onFinish;

    bool readLocalCar(CarState& out) override {
        if (!hasCar) return false;
        out = car;
        return true;
    }
    u32 zoneId() const override { return zone; }

    void readGameEntities(std::vector<GameEntityView>& out) override {
        if (!simulateTraffic) return;
        out.reserve(traffic.size());
        for (const Traffic& t : traffic) {
            GameEntityView v;
            v.kind = EKind::Traffic;
            v.model = t.model;
            v.handle = t.handle;
            v.st.pos = {t.x, 0, t.z};
            v.st.yaw = 0;
            v.st.gear = 1;
            v.st.vel = {t.speed, 0, 0};
            out.push_back(v);
        }
    }

    void onSpawn(EntityId id, EKind kind, u8 model, const CarState& st) override {
        Puppet p;
        p.kind = kind;
        p.model = model;
        p.st = st;
        puppets[id] = p;
        ++spawns;
    }
    void onUpdate(EntityId id, EKind kind, const CarState& st) override {
        auto it = puppets.find(id);
        if (it == puppets.end()) {
            onSpawn(id, kind, st.model, st);
            return;
        }
        it->second.st = st;
        ++it->second.updates;
    }
    void onDespawn(EntityId id, EKind kind) override {
        (void)kind;
        puppets.erase(id);
        ++despawns;
    }

    void onRace(const RaceState& race, RacePhase previous) override {
        (void)previous;
        ++raceCallbacks;
        phase = race.phase;
        if (race.phase == RacePhase::Countdown) {
            racing = false;
            finished = false;
            lastCheckpoint = 0;
            car.pos = {0, 0, 0};
            car.vel = {0, 0, 0};
        } else if (race.phase == RacePhase::Racing) {
            racing = true;
            finished = false;
            lastCheckpoint = 0;
            raceStartHostMs = race.startAtMs;
            car.pos = {0, 0, 0};
        } else if (race.phase == RacePhase::Finished || race.phase == RacePhase::Aborted) {
            racing = false;
        }
    }

    void onCareer(const ProgressLedger& l) override { ledger = l; }

    void onNotice(const std::string& text) override {
        notices.push_back(text);
        if (notices.size() > 64) notices.erase(notices.begin());
    }

    void onConnection(bool c, const std::string& reason) override {
        connected = c;
        connectionReason = reason;
    }

    // Advance the world. `hostNow` is the host's clock on every machine, so all
    // cars are timed against the same GO.
    void step(u32 hostNow) {
        if (simulateTraffic) {
            for (Traffic& t : traffic) {
                t.x = std::fmod(t.x + t.speed * 0.005f, 400.0f);
            }
        }
        if (!racing || finished) return;
        const u32 elapsed = hostNow > raceStartHostMs ? hostNow - raceStartHostMs : 0;
        car.pos.x = speed * static_cast<f32>(elapsed) / 1000.0f;
        car.vel = {speed, 0, 0};
        car.gear = 2;
        const u16 cp = static_cast<u16>(std::min<f32>(static_cast<f32>(checkpointCount),
                                                     std::floor(car.pos.x / cpSpacing)));
        while (lastCheckpoint < cp) {
            ++lastCheckpoint;
            if (onProgress) onProgress(lastCheckpoint, 0, elapsed);
        }
        if (!finished && car.pos.x >= cpSpacing * static_cast<f32>(checkpointCount)) {
            finished = true;
            racing = false;
            if (onFinish) onFinish(elapsed);
        }
    }

    void addTraffic(int n, u32 baseHandle = 0x5000) {
        simulateTraffic = true;
        for (int i = 0; i < n; ++i) {
            Traffic t;
            t.handle = baseHandle + static_cast<u32>(i);
            t.model = static_cast<u8>(10 + i);
            t.x = 5.0f + 12.0f * i;
            t.z = 3.0f;
            traffic.push_back(t);
        }
        trafficCount = n;
    }
};

}  // namespace nfc
