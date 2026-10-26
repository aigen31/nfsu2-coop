// Test cluster: one host session plus N client sessions over real UDP on
// loopback, each with its own fake game world. Used by sync_tests and net_tests.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/session.h"
#include "fake_world.h"
#include "minitest.h"

namespace nfc {

struct Guest {
    FakeWorld game;
    std::unique_ptr<ClientSession> client;
};

struct Cluster {
    u16 port = 0;
    FakeWorld hostGame;
    std::unique_ptr<HostSession> host;
    std::vector<std::unique_ptr<Guest>> guests;
    SessionConfig hostCfg;
    SessionConfig clientCfg;
    std::function<void()> extraPump;  // e.g. the chaos proxy

    explicit Cluster(u16 p) : port(p) {
        hostCfg.port = p;
        hostCfg.hostMode = true;
        hostCfg.playerName = "host";
        hostCfg.countdownMs = 300;
        hostCfg.lobbySettleMs = 100;
        hostCfg.snapshotHz = 30;
        clientCfg.serverHost = "127.0.0.1";
        clientCfg.serverPort = p;
        clientCfg.countdownMs = 300;
        clientCfg.lobbySettleMs = 100;
        clientCfg.snapshotHz = 30;
    }
    ~Cluster() { shutdown(); }

    void startHost() {
        host = std::make_unique<HostSession>(hostGame, hostCfg);
        CHECK(host->start());
        HostSession* h = host.get();
        hostGame.onProgress = [h](u16 cp, u16 lap, u32 t) { h->reportLocalProgress(cp, lap, t); };
        hostGame.onFinish = [h](u32 t) { h->reportLocalFinished(t); };
    }

    Guest& addGuest(const std::string& name, u16 connectPortOverride = 0) {
        guests.push_back(std::make_unique<Guest>());
        Guest& g = *guests.back();
        SessionConfig cfg = clientCfg;
        cfg.playerName = name;
        if (connectPortOverride) cfg.serverPort = connectPortOverride;
        g.game.simulateTraffic = false;
        g.client = std::make_unique<ClientSession>(g.game, cfg);
        ClientSession* c = g.client.get();
        g.game.onProgress = [c](u16 cp, u16 lap, u32 t) { c->reportProgress(cp, lap, t); };
        g.game.onFinish = [c](u32 t) { c->reportFinished(t); };
        CHECK(g.client->start());
        return g;
    }

    void pump() {
        if (extraPump) extraPump();
        const u32 t = static_cast<u32>(nowMs());
        hostGame.step(t);
        for (auto& g : guests) g->game.step(g->client->hostNowMs(t));
        if (host) host->tick(t);
        for (auto& g : guests) g->client->tick(t);
        sleepMs(3);
    }

    bool runUntil(const std::function<bool()>& pred, int timeoutMs) {
        const u64 end = nowMs() + static_cast<u64>(timeoutMs);
        while (nowMs() < end) {
            pump();
            if (pred()) return true;
        }
        return pred();
    }

    void run(int ms) {
        const u64 end = nowMs() + static_cast<u64>(ms);
        while (nowMs() < end) pump();
    }

    void shutdown() {
        for (auto& g : guests)
            if (g->client) g->client->stop();
        if (host) host->stop();
        guests.clear();
    }
};

inline void setRoute(FakeWorld& w, u16 checkpoints, f32 spacing, f32 speed) {
    w.checkpointCount = checkpoints;
    w.cpSpacing = spacing;
    w.speed = speed;
}

}  // namespace nfc
