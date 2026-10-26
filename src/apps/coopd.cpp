// coopd: dedicated co-op host. No game needed: it runs the race director, the
// shared career ledger and the relay for every player's car.
//
//   coopd --port 27970 [--name "career"] [--no-traffic]
//
// Commands: status, players, race start <event> <checkpoints>, race abort,
// career <type> <id>, say <text>, kick <slot>, resync <slot>, quit
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#endif

#include "core/log.h"
#include "core/session.h"
#include "core/types.h"

using namespace nfc;

namespace {

class HeadlessWorld : public IGameWorld {
public:
    bool readLocalCar(CarState&) override { return false; }
    u32 zoneId() const override { return 0; }
    void readGameEntities(std::vector<GameEntityView>&) override {}
    void onSpawn(EntityId, EKind, u8, const CarState&) override {}
    void onUpdate(EntityId, EKind, const CarState&) override {}
    void onDespawn(EntityId, EKind) override {}
    void onRace(const RaceState&, RacePhase) override {}
    void onCareer(const ProgressLedger& l) override {
        std::printf("[career] rev %u  cash %u  rep %u\n", l.revision, l.cash, l.rep);
    }
    void onNotice(const std::string& text) override { std::printf("[notice] %s\n", text.c_str()); }
};

void usage() {
    std::printf(
        "coopd %s - dedicated NFS Underground 2 co-op host\n"
        "  --port N        UDP port (default 27970)\n"
        "  --name NAME     host name shown to players\n"
        "  --session NAME  session name\n"
        "  --no-traffic    do not relay traffic/bot cars\n"
        "  --countdown MS  race countdown (default 5000)\n"
        "  --auto-race N   open event N after 1.5 s (for automated runs)\n"
        "  --seconds N     stop after N seconds (0 = forever)\n"
        "  --log FILE      log file (default coopd.log)\n",
        versionString());
}

}  // namespace

int main(int argc, char** argv) {
    SessionConfig cfg;
    cfg.hostMode = true;
    cfg.playerName = "dedicated";
    cfg.publishGameEntities = false;
    std::string logPath = "coopd.log";
    bool noTraffic = false;
    unsigned autoRace = 0;
    u32 runSeconds = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--port") && i + 1 < argc)
            cfg.port = static_cast<u16>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--name") && i + 1 < argc)
            cfg.playerName = argv[++i];
        else if (!std::strcmp(a, "--session") && i + 1 < argc)
            cfg.sessionName = argv[++i];
        else if (!std::strcmp(a, "--countdown") && i + 1 < argc)
            cfg.countdownMs = static_cast<u32>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--auto-race") && i + 1 < argc)
            autoRace = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--seconds") && i + 1 < argc)
            runSeconds = static_cast<u32>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--no-traffic"))
            noTraffic = true;
        else if (!std::strcmp(a, "--log") && i + 1 < argc)
            logPath = argv[++i];
        else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            usage();
            return 0;
        }
    }
    cfg.publishGameEntities = !noTraffic;

    logOpen(logPath, LogLevel::Info, true);
    NFC_INFO("coopd %s starting", versionString());

    HeadlessWorld world;
    HostSession host(world, cfg);
    if (!host.start()) {
        std::printf("cannot bind udp/%u (already in use?)\n", cfg.port);
        return 1;
    }
    std::printf("coopd listening on udp/%u, session '%s'. type 'help' for commands.\n", cfg.port,
                cfg.sessionName.c_str());

    std::string line;
    bool quit = false;
    u64 lastPrint = 0;
    const u64 started = nowMs();
    bool autoOpened = false;
    while (!quit) {
        if (autoRace && !autoOpened && nowMs() - started > 1500) {
            autoOpened = true;
            if (host.openRace(static_cast<u16>(autoRace), 4001, 10))
                std::printf("auto: event %u open\n", autoRace);
        }
        if (runSeconds && nowMs() - started > static_cast<u64>(runSeconds) * 1000) {
            std::printf("auto: run time over\n");
            quit = true;
        }
        host.tick(static_cast<u32>(nowMs()));
#ifdef _WIN32
        while (_kbhit()) {
            const int ch = _getch();
            if (ch == '\r' || ch == '\n') {
                std::printf("> %s\n", line.c_str());
                if (line == "quit" || line == "exit") {
                    quit = true;
                } else if (line == "status") {
                    std::printf("%s\n", host.statusText().c_str());
                    std::printf("  race: %s event %u  version %u\n", toString(host.world().race.phase),
                                host.world().race.eventId, host.world().race.version);
                    std::printf("  career: rev %u cash %u rep %u\n", host.world().progress.revision,
                                host.world().progress.cash, host.world().progress.rep);
                } else if (line == "players") {
                    std::printf("%s", host.world().playerList().c_str());
                } else if (line.rfind("race start ", 0) == 0) {
                    unsigned ev = 0, cps = 10;
                    std::sscanf(line.c_str() + 11, "%u %u", &ev, &cps);
                    if (host.openRace(static_cast<u16>(ev), 4001, static_cast<u16>(cps)))
                        std::printf("race %u open, waiting for ready\n", ev);
                    else
                        std::printf("cannot open a race now\n");
                } else if (line == "race go") {
                    std::printf(host.forceStart() ? "countdown started\n" : "no lobby open\n");
                } else if (line == "race abort") {
                    std::printf(host.abortRace() ? "race aborted\n" : "no race to abort\n");
                } else if (line.rfind("career ", 0) == 0) {
                    unsigned type = 0, id = 0;
                    if (std::sscanf(line.c_str() + 7, "%u %u", &type, &id) == 2)
                        std::printf(host.applyCareer(careerKey(type, id)) ? "career event applied\n"
                                                                          : "duplicate or invalid event\n");
                } else if (line.rfind("say ", 0) == 0) {
                    host.chat(line.substr(4));
                } else if (line.rfind("kick ", 0) == 0) {
                    host.kick(static_cast<PlayerId>(std::atoi(line.c_str() + 5)));
                } else if (line.rfind("resync", 0) == 0) {
                    host.resync(kNoPlayer);
                    std::printf("broadcast race and career state\n");
                } else if (line == "help") {
                    std::printf(
                        "status | players | race start <event> <checkpoints> | race go | race abort\n"
                        "career <type> <id> | say <text> | kick <slot> | resync | quit\n");
                } else if (!line.empty()) {
                    std::printf("unknown command: %s\n", line.c_str());
                }
                line.clear();
            } else if (ch == 8) {
                if (!line.empty()) line.pop_back();
            } else if (ch >= 32 && ch < 127) {
                line.push_back(static_cast<char>(ch));
            }
        }
#endif
        const u64 n = nowMs();
        if (n - lastPrint > 5000) {
            lastPrint = n;
            NFC_DEBUG("%s", host.statusText().c_str());
        }
        sleepMs(5);
    }

    host.stop();
    logClose();
    return 0;
}


