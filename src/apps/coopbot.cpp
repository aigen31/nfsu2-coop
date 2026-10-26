// coop-bot: a headless co-op client. Joins a host, readies up, drives a
// synthetic car, reports checkpoints and the finish, and prints a JSON summary.
// This is the "bot in the game" of the QA playbook, minus the renderer: the
// session code it exercises is the same the plugin runs.
//
//   coop-bot --connect 127.0.0.1:27970 --name bot1 --race --seconds 20
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/session.h"
#include "core/types.h"

using namespace nfc;

namespace {

class BotWorld : public IGameWorld {
public:
    CarState car;
    bool racing = false;
    bool finished = false;
    u16 lastCheckpoint = 0;
    u32 raceStartHostMs = 0;
    f32 speed = 380.0f;
    f32 cpSpacing = 10.0f;
    int puppets = 0;
    int spawns = 0;
    int despawns = 0;
    ProgressLedger ledger;
    RacePhase phase = RacePhase::Idle;

    bool readLocalCar(CarState& out) override {
        out = car;
        return true;
    }
    u32 zoneId() const override { return 1; }
    void readGameEntities(std::vector<GameEntityView>&) override {}
    void onSpawn(EntityId, EKind, u8, const CarState&) override { ++spawns; }
    void onUpdate(EntityId, EKind, const CarState&) override {}
    void onDespawn(EntityId, EKind) override { ++despawns; }

    void onRace(const RaceState& race, RacePhase) override {
        phase = race.phase;
        if (race.phase == RacePhase::Countdown) {
            racing = false;
            finished = false;
            lastCheckpoint = 0;
            car = CarState{};
        } else if (race.phase == RacePhase::Racing) {
            racing = true;
            finished = false;
            lastCheckpoint = 0;
            raceStartHostMs = race.startAtMs;
            car = CarState{};
        } else if (race.phase == RacePhase::Finished || race.phase == RacePhase::Aborted) {
            racing = false;
        }
        checkpointCount_ = race.checkpointCount;
    }
    void onCareer(const ProgressLedger& l) override { ledger = l; }
    void onNotice(const std::string& text) override { std::printf("[notice] %s\n", text.c_str()); }
    void onConnection(bool c, const std::string& reason) override {
        std::printf("[conn] %s %s\n", c ? "up" : "down", reason.c_str());
    }

    void step(u32 hostNow) {
        if (!racing || finished) return;
        const u32 elapsed = hostNow > raceStartHostMs ? hostNow - raceStartHostMs : 0;
        car.pos.x = speed * static_cast<f32>(elapsed) / 1000.0f;
        car.vel = {speed, 0, 0};
        car.gear = 3;
        const u16 cp = static_cast<u16>(std::min<f32>(static_cast<f32>(checkpointCount_),
                                                      std::floor(car.pos.x / cpSpacing)));
        for (u16 c = static_cast<u16>(lastCheckpoint + 1); c <= cp; ++c) {
            lastCheckpoint = c;
            if (onProgress) onProgress(c, 0, elapsed);
        }
        if (!finished && car.pos.x >= cpSpacing * static_cast<f32>(checkpointCount_)) {
            finished = true;
            racing = false;
            if (onFinish) onFinish(elapsed);
        }
    }

    std::function<void(u16, u16, u32)> onProgress;
    std::function<void(u32)> onFinish;

private:
    u16 checkpointCount_ = 10;
};

void usage() {
    std::printf(
        "coop-bot %s - headless co-op client\n"
        "  --connect HOST:PORT  host or chaos relay (default 127.0.0.1:27970)\n"
        "  --name NAME          player name\n"
        "  --seconds N          run time before giving up (default 30)\n"
        "  --race               ready up for any race the host opens\n"
        "  --speed N            synthetic car speed (default 380 m/s, tests only)\n"
        "  --json FILE          write the summary there as well\n"
        "  --log FILE           log file (default coop-bot.log)\n",
        versionString());
}

}  // namespace

int main(int argc, char** argv) {
    SessionConfig cfg;
    cfg.playerName = "bot";
    u32 seconds = 30;
    bool raceMode = false;
    std::string logPath = "coop-bot.log";
    std::string jsonPath;
    f32 speed = 380.0f;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--connect") && i + 1 < argc) {
            const std::string spec = argv[++i];
            const std::size_t colon = spec.find(':');
            if (colon == std::string::npos) {
                cfg.serverHost = spec;
            } else {
                cfg.serverHost = spec.substr(0, colon);
                cfg.serverPort = static_cast<u16>(std::atoi(spec.c_str() + colon + 1));
            }
        } else if (!std::strcmp(a, "--name") && i + 1 < argc) {
            cfg.playerName = argv[++i];
        } else if (!std::strcmp(a, "--seconds") && i + 1 < argc) {
            seconds = static_cast<u32>(std::atoi(argv[++i]));
        } else if (!std::strcmp(a, "--race")) {
            raceMode = true;
        } else if (!std::strcmp(a, "--speed") && i + 1 < argc) {
            speed = static_cast<f32>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--json") && i + 1 < argc) {
            jsonPath = argv[++i];
        } else if (!std::strcmp(a, "--log") && i + 1 < argc) {
            logPath = argv[++i];
        } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            usage();
            return 0;
        }
    }

    logOpen(logPath, LogLevel::Info, false);
    std::printf("coop-bot '%s' -> %s:%u\n", cfg.playerName.c_str(), cfg.serverHost.c_str(), cfg.serverPort);

    BotWorld world;
    world.speed = speed;
    ClientSession client(world, cfg);
    world.onProgress = [&client](u16 cp, u16 lap, u32 t) { client.reportProgress(cp, lap, t); };
    world.onFinish = [&client](u32 t) { client.reportFinished(t); };

    if (!client.start()) {
        std::printf("cannot start a UDP client\n");
        return 1;
    }

    const u64 start = nowMs();
    const u64 deadline = start + static_cast<u64>(seconds) * 1000;
    u16 lastReadyEvent = 0xFFFF;
    bool sawRace = false;
    bool raceFinished = false;

    while (nowMs() < deadline) {
        const u32 t = static_cast<u32>(nowMs());
        world.step(client.hostNowMs(t));
        client.tick(t);

        const RaceState& race = client.world().race;
        if (raceMode && client.connected() && race.phase == RacePhase::Lobby && race.eventId != lastReadyEvent) {
            lastReadyEvent = race.eventId;
            client.setReady(true);
            std::printf("[race] ready for event %u\n", race.eventId);
        }
        if (race.phase == RacePhase::Racing) sawRace = true;
        if (race.phase == RacePhase::Finished && race.eventId != 0) raceFinished = true;
        if (raceMode && raceFinished) break;
        sleepMs(5);
    }

    const RaceState& race = client.world().race;
    std::printf("{\"name\":\"%s\",\"connected\":%s,\"slot\":%u,\"racePhase\":\"%s\",\"raceEvent\":%u,"
                "\"ledgerRevision\":%u,\"cash\":%u,\"rep\":%u,\"progressDigest\":%llu,\"spawns\":%d,"
                "\"rtt\":%u,\"sawRace\":%s,\"raceFinished\":%s}\n",
                cfg.playerName.c_str(), client.connected() ? "true" : "false", client.localSlot(),
                toString(race.phase), race.eventId, world.ledger.revision, world.ledger.cash, world.ledger.rep,
                static_cast<unsigned long long>(world.ledger.digest()), world.spawns, client.rttMs(),
                sawRace ? "true" : "false", raceFinished ? "true" : "false");
    if (!jsonPath.empty()) {
        if (FILE* f = std::fopen(jsonPath.c_str(), "wb")) {
            std::fprintf(f,
                         "{\"name\":\"%s\",\"connected\":%s,\"slot\":%u,\"racePhase\":\"%s\",\"raceEvent\":%u,"
                         "\"ledgerRevision\":%u,\"cash\":%u,\"rep\":%u,\"progressDigest\":%llu,\"spawns\":%d,"
                         "\"rtt\":%u,\"sawRace\":%s,\"raceFinished\":%s}\n",
                         cfg.playerName.c_str(), client.connected() ? "true" : "false", client.localSlot(),
                         toString(race.phase), race.eventId, world.ledger.revision, world.ledger.cash,
                         world.ledger.rep, static_cast<unsigned long long>(world.ledger.digest()), world.spawns,
                         client.rttMs(), sawRace ? "true" : "false", raceFinished ? "true" : "false");
            std::fclose(f);
        }
    }

    const bool ok = client.connected() && (!raceMode || raceFinished);
    client.stop();
    logClose();
    return ok ? 0 : 3;
}

