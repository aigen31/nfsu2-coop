// Network tests: real UDP through a chaos proxy that drops and duplicates
// datagrams. Proves the reliable channel and the digest/repair path converge
// on a bad link, which is where co-op sync normally breaks.
#include "chaos_proxy.h"

#include "core/log.h"

#include <cmath>

#include "cluster.h"
#include "minitest.h"

using namespace nfc;

TEST(lossy_link_converges) {
    const u16 hostPort = 28201;
    const u16 proxyPort = 28202;
    ChaosProxy proxy;
    CHECK(proxy.start(proxyPort, hostPort, 25, 10));

    Cluster c(hostPort);
    c.extraPump = [&] { proxy.pump(); };
    c.hostGame.addTraffic(6);
    c.startHost();
    setRoute(c.hostGame, 6, 10, 400);
    Guest& g = c.addGuest("lossy", proxyPort);
    setRoute(g.game, 6, 10, 400);

    CHECK(c.runUntil([&] { return g.client->connected(); }, 10000));
    CHECK(c.runUntil([&] { return c.host->world().players[1].connected; }, 5000));

    g.game.car.pos = {33, 0, 0};
    c.run(900);
    const Entity* e = c.host->world().find(c.host->world().players[1].carId);
    CHECK(e != nullptr);
    if (e) CHECK_NEAR(e->st.pos.x, 33.0f, 5.0f);
    CHECK(c.runUntil([&] { return g.game.puppets.size() >= 6; }, 5000));

    CHECK(c.host->openRace(21, 4005, 6));
    CHECK(c.runUntil([&] { return g.client->world().race.phase == RacePhase::Lobby; }, 5000));
    CHECK(g.client->setReady(true));
    CHECK(c.runUntil([&] { return c.host->world().race.phase == RacePhase::Finished; }, 12000));
    CHECK(c.runUntil([&] { return g.game.ledger.revision == 1; }, 5000));

    CHECK_EQ(g.game.ledger.digest(), c.host->world().progress.digest());
    CHECK(c.runUntil([&] { return g.client->world().race.digest() == c.host->world().race.digest(); }, 5000));
    CHECK(proxy.dropped() > 0);
    CHECK(proxy.received() > 0);
    NFC_INFO("net: lossy test dropped %d datagrams", proxy.dropped());
}

TEST(partition_then_recovery) {
    const u16 hostPort = 28203;
    const u16 proxyPort = 28204;
    ChaosProxy proxy;
    CHECK(proxy.start(proxyPort, hostPort, 0, 0));

    Cluster c(hostPort);
    c.extraPump = [&] { proxy.pump(); };
    c.hostGame.addTraffic(3);
    c.startHost();
    setRoute(c.hostGame, 6, 10, 400);
    Guest& g = c.addGuest("cut", proxyPort);
    setRoute(g.game, 6, 10, 400);
    CHECK(c.runUntil([&] { return g.client->connected(); }, 5000));

    // The link goes away for 1.2 s: both sides must survive and re-converge.
    proxy.setDrop(100);
    c.run(1200);
    proxy.setDrop(0);

    CHECK(c.runUntil([&] { return g.client->connected(); }, 9000));
    c.run(1200);
    CHECK(c.runUntil([&] { return g.game.puppets.size() >= 3; }, 6000));
    CHECK_EQ(g.client->world().progress.revision, c.host->world().progress.revision);
}

int main() { return ::runAll("net"); }


