// Session-level sync tests: a real host session and real client sessions, real
// UDP sockets on loopback, fake game worlds. No game install needed.
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/session.h"
#include "core/types.h"
#include "fake_world.h"
#include "minitest.h"

using namespace nfc;

namespace {

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
    int digestMismatchMax = 0;

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
        g.game.simulateTraffic = false;  // a guest shows puppets, not its own traffic
        g.client = std::make_unique<ClientSession>(g.game, cfg);
        ClientSession* c = g.client.get();
        g.game.onProgress = [c](u16 cp, u16 lap, u32 t) { c->reportProgress(cp, lap, t); };
        g.game.onFinish = [c](u32 t) { c->reportFinished(t); };
        CHECK(g.client->start());
        return g;
    }

    void pump() {
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
        for (auto& g : guests) {
            if (g->client) g->client->stop();
        }
        if (host) host->stop();
        guests.clear();
    }
};

void setRoute(FakeWorld& w, u16 checkpoints, f32 spacing, f32 speed) {
    w.checkpointCount = checkpoints;
    w.cpSpacing = spacing;
    w.speed = speed;
}

}  // namespace

TEST(guest_and_host_see_each_other) {
    Cluster c(28101);
    c.startHost();
    setRoute(c.hostGame, 10, 10, 400);
    Guest& g = c.addGuest("guest1");
    setRoute(g.game, 10, 10, 400);

    CHECK(c.runUntil([&] { return g.client->connected(); }, 2000));
    CHECK(c.runUntil([&] { return c.host->world().players[1].connected; }, 2000));
    CHECK_EQ(c.host->world().connectedCount(), 2);

    g.game.car.pos = {40, 0, 0};
    c.run(300);

    const EntityId guestCar = c.host->world().players[1].carId;
    CHECK(guestCar != kNoEntity);
    const Entity* e = c.host->world().find(guestCar);
    CHECK(e != nullptr);
    if (e) CHECK_NEAR(e->st.pos.x, 40.0f, 1.0f);

    const EntityId hostCar = c.host->world().players[0].carId;
    CHECK_EQ(g.game.puppets.count(hostCar), static_cast<std::size_t>(1));
}

TEST(traffic_is_mirrored_to_guests) {
    Cluster c(28102);
    c.hostGame.addTraffic(4);
    c.startHost();
    setRoute(c.hostGame, 10, 10, 400);
    Guest& g = c.addGuest("guest1");

    CHECK(c.runUntil([&] { return g.client->connected(); }, 2000));
    c.run(700);
    CHECK_EQ(c.host->world().activeCount(EKind::Traffic), static_cast<std::size_t>(4));
    CHECK(g.game.puppets.size() >= 4);

    const int before = g.game.despawns;
    c.hostGame.traffic.pop_back();
    c.run(1500);
    CHECK(g.game.despawns > before);
    CHECK_EQ(c.host->world().activeCount(EKind::Traffic), static_cast<std::size_t>(3));
}

TEST(race_end_to_end) {
    Cluster c(28103);
    c.startHost();
    setRoute(c.hostGame, 10, 10, 400);
    Guest& a = c.addGuest("guest1");
    Guest& b = c.addGuest("guest2");
    setRoute(a.game, 10, 10, 400);
    setRoute(b.game, 10, 10, 400);

    CHECK(c.runUntil([&] { return a.client->connected() && b.client->connected(); }, 3000));
    CHECK(c.host->openRace(7, 4001, 10));
    CHECK(c.runUntil([&] { return a.client->world().race.phase == RacePhase::Lobby; }, 2000));
    CHECK(a.client->setReady(true));
    CHECK(b.client->setReady(true));

    CHECK(c.runUntil([&] { return c.host->world().race.phase == RacePhase::Racing; }, 3000));
    CHECK(c.runUntil([&] { return c.host->world().race.phase == RacePhase::Finished; }, 5000));

    const RaceState& r = c.host->world().race;
    CHECK_EQ(r.finisherCount, static_cast<u8>(3));
    CHECK(r.p[0].finished);
    CHECK(r.p[1].finished);
    CHECK(r.p[2].finished);
    CHECK(r.p[0].place == 1);
    CHECK(r.p[1].place == 2);
    CHECK(r.p[2].place == 3);

    // Career was applied once, and both guests ended with the host's ledger.
    const u32 key = careerKey(kCareerRace, 7);
    CHECK(c.host->world().progress.has(key));
    CHECK_EQ(c.host->world().progress.revision, 1u);
    CHECK(c.runUntil([&] { return a.game.ledger.revision == 1 && b.game.ledger.revision == 1; }, 2000));
    CHECK_EQ(a.game.ledger.digest(), c.host->world().progress.digest());
    CHECK_EQ(b.game.ledger.digest(), c.host->world().progress.digest());
    CHECK_EQ(a.game.ledger.cash, careerEffect(key).cashDelta);

    // The guests saw the results phase too.
    CHECK(c.runUntil([&] { return a.client->world().race.phase == RacePhase::Finished; }, 2000));
    CHECK_EQ(a.client->world().race.digest(), c.host->world().race.digest());

    CHECK_EQ(a.client->stats().digestMismatches <= 4, true);
}

TEST(career_events_are_applied_once) {
    Cluster c(28104);
    c.startHost();
    setRoute(c.hostGame, 10, 10, 400);
    Guest& g = c.addGuest("guest1");
    CHECK(c.runUntil([&] { return g.client->connected(); }, 2000));

    const u32 key = careerKey(kCareerOutrun, 3);
    g.client->reportCareerEvent(key);
    c.run(150);
    g.client->reportCareerEvent(key);  // duplicate: must not pay twice
    c.run(400);

    CHECK(c.host->world().progress.has(key));
    CHECK_EQ(c.host->world().progress.revision, 1u);
    CHECK_EQ(c.host->world().progress.cash, careerEffect(key).cashDelta);
    CHECK_EQ(g.game.ledger.revision, 1u);
    CHECK_EQ(g.game.ledger.cash, careerEffect(key).cashDelta);
}

TEST(late_joiner_gets_the_whole_state) {
    Cluster c(28105);
    c.startHost();
    setRoute(c.hostGame, 10, 10, 400);
    Guest& a = c.addGuest("guest1");
    CHECK(c.runUntil([&] { return a.client->connected(); }, 2000));
    CHECK(c.host->openRace(9, 4002, 10));
    CHECK(c.runUntil([&] { return a.client->world().race.phase == RacePhase::Lobby; }, 2000));
    a.client->setReady(true);
    CHECK(c.runUntil([&] { return c.host->world().race.phase == RacePhase::Racing; }, 3000));

    // The race is running, the career already has an event, and a new player joins.
    c.host->applyCareer(careerKey(kCareerDvd, 1));
    Guest& b = c.addGuest("late");
    setRoute(b.game, 10, 10, 400);
    CHECK(c.runUntil([&] { return b.client->connected(); }, 3000));
    CHECK(c.runUntil([&] { return b.client->world().race.phase == RacePhase::Racing; }, 2000));
    CHECK(c.runUntil([&] { return b.game.ledger.revision == 1; }, 2000));
    CHECK(b.client->world().progress.has(careerKey(kCareerDvd, 1)));
    CHECK_EQ(b.client->world().race.eventId, static_cast<u16>(9));
}

TEST(host_stop_drops_guests) {
    Cluster c(28106);
    c.startHost();
    setRoute(c.hostGame, 10, 10, 400);
    Guest& g = c.addGuest("guest1");
    CHECK(c.runUntil([&] { return g.client->connected(); }, 2000));
    c.host->stop();
    CHECK(c.runUntil([&] { return !g.client->connected(); }, 3000));
}

int main() { return ::runAll("sync"); }
