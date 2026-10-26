// The game side of the mod: reads the player car, mirrors the engine's own
// traffic and AI cars, and turns race/career decisions into game actions.
// Every access goes through a small safe-read helper, and game functions are
// queued and run on the render thread (the game's own thread).
#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "core/session.h"
#include "core/types.h"
#include "hud.h"
#include "scanner.h"

namespace nfc {

class Nfsu2Game : public IGameWorld {
public:
    bool init(HMODULE module, const GameConfig& cfg);
    void shutdown();

    bool readLocalCar(CarState& out) override;
    u32 zoneId() const override;
    void readGameEntities(std::vector<GameEntityView>& out) override;

    void onSpawn(EntityId id, EKind kind, u8 model, const CarState& st) override;
    void onUpdate(EntityId id, EKind kind, const CarState& st) override;
    void onDespawn(EntityId id, EKind kind) override;
    void onRace(const RaceState& race, RacePhase previous) override;
    void onCareer(const ProgressLedger& ledger) override;
    void onNotice(const std::string& text) override;
    void onConnection(bool connected, const std::string& reason) override;

    // Runs queued game calls. Called from the D3D9 EndScene hook.
    void pumpGameThread();

    bool bound() const { return b_.carBound(); }
    u32 carPointer() const { return lastCarPtr_; }
    u32 carAt(u32 index) const;
    u32 renderVa() const;
    bool trafficBound() const { return b_.trafficBound(); }
    const GameBindings& bindings() const { return b_; }
    const std::vector<std::string>& notices() const { return notices_; }
    int puppetCount() const { return static_cast<int>(puppets_.size()); }
    int trafficCount() const { return traffic_; }
    const RaceState& lastRace() const { return race_; }
    const ProgressLedger& ledger() const { return ledger_; }
    std::string modulePath() const { return modulePath_; }

private:
    u8* at(u32 va) const;
    static bool safeRead(const void* addr, void* out, std::size_t n);
    void note(const std::string& text);
    void queue(std::function<void()> fn);

    HMODULE module_ = nullptr;
    std::string modulePath_;
    PeImage image_;
    GameBindings b_;
    GameConfig cfg_;
    RaceState race_;
    ProgressLedger ledger_;
    std::map<EntityId, EKind> puppets_;
    int traffic_ = 0;
    std::vector<std::string> notices_;
    std::mutex queueMutex_;
    std::vector<std::function<void()>> pending_;
    u16 lastStartedEvent_ = 0xFFFF;
    u32 lastCarPtr_ = 0;
    bool warnedUnbound_ = false;
    bool warnedStart_ = false;
};

}  // namespace nfc


