// Core logic tests: wire format, RNG, career ledger, race rules, digests.
// No sockets, no engine: these run in milliseconds.
#include <cmath>
#include <cstdio>

#include "core/protocol.h"
#include "core/types.h"
#include "minitest.h"

using namespace nfc;

namespace {

World makeWorld() {
    World w;
    w.reset();
    w.sessionSeed = 0x12345678;
    w.players[0].connected = true;
    w.players[0].loaded = true;
    w.players[0].name = "host";
    w.players[0].carId = 1;
    w.players[1].connected = true;
    w.players[1].loaded = true;
    w.players[1].name = "guest";
    w.players[1].carId = 2;
    return w;
}

}  // namespace

TEST(car_state_roundtrip) {
    CarState a;
    a.pos = {123.5f, -7.25f, 999.0f};
    a.vel = {10.5f, 0.0f, -2.25f};
    a.yaw = 1.234f;
    a.pitch = 0.1f;
    a.roll = -0.2f;
    a.steer = -0.5f;
    a.gear = 3;
    a.model = 17;
    a.flags = kCarNos | kCarDrift;

    BitWriter w;
    a.encode(w);
    CHECK_EQ(w.size(), static_cast<std::size_t>(CarState::kWireSize));

    BitReader r(w);
    CarState b;
    CHECK(b.decode(r));
    CHECK(r.ok());
    CHECK_NEAR(b.pos.x, a.pos.x, 0.02f);
    CHECK_NEAR(b.pos.y, a.pos.y, 0.02f);
    CHECK_NEAR(b.pos.z, a.pos.z, 0.02f);
    CHECK_NEAR(b.vel.z, a.vel.z, 0.01f);
    CHECK_NEAR(b.yaw, a.yaw, 0.001f);
    CHECK_NEAR(b.pitch, a.pitch, 0.001f);
    CHECK_NEAR(b.roll, a.roll, 0.001f);
    CHECK_NEAR(b.steer, a.steer, 0.01f);
    CHECK_EQ(b.gear, a.gear);
    CHECK_EQ(b.model, a.model);
    CHECK_EQ(b.flags, a.flags);
}

TEST(car_state_rejects_garbage) {
    CarState a;
    a.pos = {0, 0, 0};
    CHECK(a.sane());
    a.pos.x = std::nanf("");
    CHECK(!a.sane());
    a.pos.x = 1.0e9f;
    CHECK(!a.sane());
}

TEST(reader_survives_truncation) {
    CarState a;
    a.pos = {1, 2, 3};
    BitWriter w;
    a.encode(w);
    for (std::size_t cut = 0; cut < w.size(); cut += 5) {
        BitReader r(w.data(), cut);
        CarState b;
        const bool ok = b.decode(r);
        CHECK(!ok);
        CHECK(!r.ok());
    }
}

TEST(rng_is_deterministic) {
    Rng a(42), b(42), c(43);
    bool same = true, differ = false;
    for (int i = 0; i < 100; ++i) {
        const u32 x = a.next32();
        if (x != b.next32()) same = false;
        if (x != c.next32()) differ = true;
    }
    CHECK(same);
    CHECK(differ);
    Rng d(42);
    CHECK_EQ(d.next32(), Rng(42).next32());
}

TEST(career_effect_is_pure) {
    const u32 race = careerKey(kCareerRace, 7);
    const CareerEffect a = careerEffect(race);
    const CareerEffect b = careerEffect(race);
    CHECK_EQ(a.cashDelta, b.cashDelta);
    CHECK_EQ(a.repDelta, b.repDelta);
    CHECK(a.cashDelta > 0);
    CHECK_EQ(careerEffect(careerKey(kCareerRace, 7)).cashDelta, a.cashDelta);
    CHECK_EQ(careerEffect(careerKey(kCareerRace, 8)).cashDelta, 500 + 100 * 3);
}

TEST(ledger_is_idempotent) {
    ProgressLedger l;
    const u32 key = careerKey(kCareerRace, 3);
    CHECK(l.apply(key));
    CHECK_EQ(l.revision, 1u);
    const u32 cash = l.cash;
    CHECK(!l.apply(key));
    CHECK_EQ(l.revision, 1u);
    CHECK_EQ(l.cash, cash);
    CHECK(l.has(key));
    CHECK_EQ(l.order.size(), static_cast<std::size_t>(1));
}

TEST(ledger_roundtrip_and_digest) {
    ProgressLedger l;
    for (u32 i = 1; i <= 5; ++i) l.apply(careerKey(kCareerRace, i));
    l.apply(careerKey(kCareerMagazine, 2));
    BitWriter w;
    l.encode(w);
    BitReader r(w);
    ProgressLedger m;
    CHECK(m.decode(r));
    CHECK_EQ(m.revision, l.revision);
    CHECK_EQ(m.cash, l.cash);
    CHECK_EQ(m.rep, l.rep);
    CHECK_EQ(m.digest(), l.digest());
    CHECK_EQ(m.order.size(), l.order.size());
}

TEST(race_rules) {
    World w = makeWorld();
    CHECK(w.race.phase == RacePhase::Idle);
    CHECK(raceOpen(w, 7, 4001, 10, 1000));
    CHECK(w.race.phase == RacePhase::Lobby);
    CHECK(w.race.p[0].joined);
    CHECK(w.race.p[1].joined);
    CHECK(!w.race.p[0].ready);

    CHECK(raceSetReady(w, 0, true, 7));
    CHECK(!raceSetReady(w, 0, true, 7));  // no change, no bump
    CHECK(raceSetReady(w, 1, true, 7));
    const u8 before = w.race.version;
    CHECK(raceBeginCountdown(w, 2000, 300));
    CHECK(w.race.phase == RacePhase::Countdown);
    CHECK(w.race.startAtMs == 2300);
    CHECK(w.race.version > before);

    raceTick(w, 2299);
    CHECK(w.race.phase == RacePhase::Countdown);
    raceTick(w, 2300);
    CHECK(w.race.phase == RacePhase::Racing);

    // A guest may advance one checkpoint at a time.
    CHECK(raceSetProgress(w, 1, 1, 0, 100));
    CHECK(!raceSetProgress(w, 1, 5, 0, 200));   // teleport: rejected
    CHECK(!raceSetProgress(w, 1, 0, 0, 250));   // backwards: rejected
    CHECK(!raceSetProgress(w, 1, 2, 3, 260));   // skipping laps: rejected
    CHECK(raceSetProgress(w, 1, 2, 0, 300));
    CHECK_EQ(w.race.p[1].checkpoint, static_cast<u16>(2));

    CHECK(raceSetFinished(w, 0, 5000));
    CHECK_EQ(w.race.p[0].place, static_cast<u8>(1));
    CHECK(w.race.phase == RacePhase::Racing);  // still waiting for slot 1
    CHECK(!raceSetFinished(w, 0, 5100));       // double finish: ignored
    CHECK(raceSetFinished(w, 1, 6000));
    CHECK_EQ(w.race.p[1].place, static_cast<u8>(2));
    CHECK(w.race.phase == RacePhase::Finished);
    CHECK(w.race.allDone());

    // Results are applied once, on the host only.
    const u32 key = careerKey(kCareerRace, 7);
    CHECK(!w.progress.has(key));
    raceApplyResults(w, 6100);
    CHECK(w.progress.has(key));
    CHECK_EQ(w.progress.cash, careerEffect(key).cashDelta);
    const u32 rev = w.progress.revision;
    raceApplyResults(w, 6200);
    CHECK_EQ(w.progress.revision, rev);
}

TEST(race_disconnect_is_dnf) {
    World w = makeWorld();
    CHECK(raceOpen(w, 9, 4002, 4, 1000));
    CHECK(raceSetReady(w, 0, true, 9));
    CHECK(raceSetReady(w, 1, true, 9));
    CHECK(raceBeginCountdown(w, 2000, 100));
    raceTick(w, 2100);
    CHECK(w.race.phase == RacePhase::Racing);
    w.players[1].connected = false;
    raceTick(w, 2200);
    CHECK(w.race.p[1].dnf);
    CHECK(w.race.phase == RacePhase::Racing);  // the host is still driving
    CHECK(raceSetFinished(w, 0, 7000));
    CHECK_EQ(w.race.p[0].place, static_cast<u8>(1));
    CHECK_EQ(w.race.p[1].place, static_cast<u8>(2));
    CHECK(w.race.phase == RacePhase::Finished);
}

TEST(race_abort_does_not_pay) {
    World w = makeWorld();
    CHECK(raceOpen(w, 11, 4003, 4, 1000));
    CHECK(raceBeginCountdown(w, 1000, 0));
    raceTick(w, 1000);
    CHECK(w.race.phase == RacePhase::Racing);
    CHECK(raceAbort(w, 1100));
    CHECK(w.race.phase == RacePhase::Aborted);
    raceApplyResults(w, 1100);
    CHECK(!w.progress.has(careerKey(kCareerRace, 11)));
}

TEST(world_digest_is_stable) {
    World a = makeWorld();
    World b = makeWorld();
    a.upsert(10, EKind::Bot).observe(CarState{{1, 2, 3}, {}, 0, 0, 0, 0, 1, 5, 0}, 100);
    b.upsert(10, EKind::Bot).observe(CarState{{1, 2, 3}, {}, 0, 0, 0, 0, 1, 5, 0}, 200);
    CHECK_EQ(a.digestAll(), b.digestAll());
    b.entities[10].st.pos.x = 1.5f;
    CHECK(a.digestAll() != b.digestAll());
    CHECK(a.digestEntities() != b.digestEntities());
}

TEST(entity_interpolation) {
    Entity e;
    CarState s;
    s.pos = {0, 0, 0};
    e.observe(s, 1000);
    s.pos = {10, 0, 0};
    e.observe(s, 1100);
    const CarState mid = e.interpolate(1100, 50);  // target 1050 -> half way
    CHECK_NEAR(mid.pos.x, 5.0f, 0.01f);
    const CarState late = e.interpolate(2000, 50);  // clamps to the newest
    CHECK_NEAR(late.pos.x, 10.0f, 0.01f);
}

TEST(packet_framing) {
    HelloMsg hello;
    hello.name = "nfsu2";
    BitWriter hw;
    hello.encode(hw);

    std::vector<u8> packet;
    CHECK(encodePacket(MsgType::Hello, hw.data(), hw.size(), packet));
    MsgType type;
    const u8* payload = nullptr;
    std::size_t len = 0;
    CHECK(decodePacket(packet.data(), packet.size(), type, payload, len));
    CHECK(type == MsgType::Hello);
    CHECK_EQ(len, hw.size());

    BitReader r(payload, len);
    HelloMsg back;
    CHECK(back.decode(r));
    CHECK_EQ(back.name, std::string("nfsu2"));

    // A flipped payload byte must fail the CRC, not reach the game.
    std::vector<u8> bad = packet;
    bad[kHeaderSize + 1] ^= 0x40;
    CHECK(!decodePacket(bad.data(), bad.size(), type, payload, len));

    // Truncated and wrong-magic packets are rejected.
    CHECK(!decodePacket(packet.data(), packet.size() - 1, type, payload, len));
    std::vector<u8> magic = packet;
    magic[0] = 'X';
    CHECK(!decodePacket(magic.data(), magic.size(), type, payload, len));

    // Oversized payloads are refused before they are framed.
    std::vector<u8> big(kMaxPayload + 1, 0xAA);
    std::vector<u8> out;
    CHECK(!encodePacket(MsgType::Snapshot, big.data(), big.size(), out));
}

TEST(race_state_roundtrip) {
    World w = makeWorld();
    raceOpen(w, 12, 4005, 6, 1000);
    raceSetReady(w, 0, true, 12);
    BitWriter bw;
    w.race.encode(bw);
    BitReader br(bw);
    RaceState copy;
    CHECK(copy.decode(br));
    CHECK_EQ(copy.digest(), w.race.digest());
    CHECK_EQ(copy.eventId, static_cast<u16>(12));
    CHECK(copy.p[0].ready);
    CHECK(copy.p[1].joined);
}

int main() { return ::runAll("core"); }


