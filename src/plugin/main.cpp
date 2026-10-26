// NFSU2Coop.asi entry point.
//
// Loaded by Ultimate ASI Loader (dinput8.dll) from <game>\scripts\. Starts one
// thread that owns the session, hooks D3D9 for the HUD, and exposes five keys.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/session.h"
#include "core/types.h"
#include "game/config.h"
#include "game/nfsu2_bindings.h"
#include "game/overlay.h"
#include "game/render_cars.h"

using nfc::u32;
using nfc::i32;

namespace {

HMODULE g_module = nullptr;
HANDLE g_thread = nullptr;
volatile LONG g_stop = 0;

std::unique_ptr<nfc::Nfsu2Game> g_game;
std::unique_ptr<nfc::HostSession> g_host;
std::unique_ptr<nfc::ClientSession> g_client;
nfc::GameConfig g_cfg;
std::string g_iniPath;
bool g_ready = false;

bool keyPressed(int vk) {
    static bool down[256] = {};
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = now && !down[vk & 0xFF];
    down[vk & 0xFF] = now;
    return pressed;
}

std::string formatTime(u32 ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u:%02u.%03u", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return buf;
}

nfc::World* world() {
    if (g_host) return &g_host->world();
    if (g_client) return &g_client->world();
    return nullptr;
}

u32 hostNowMs(u32 now) { return g_client ? g_client->hostNowMs(now) : now; }

nfc::HudState buildHud(u32 now) {
    nfc::HudState hud;
    hud.title = "NFSU2 Co-op";
    hud.host = g_cfg.host;
    hud.bound = g_game && g_game->bound();
    hud.traffic = g_game ? g_game->trafficCount() : 0;
    hud.puppets = g_game ? g_game->puppetCount() : 0;
    hud.visible = true;

    nfc::World* w = world();
    if (!w) {
        hud.status = "starting...";
        return hud;
    }
    hud.connected = g_host ? true : (g_client && g_client->connected());
    hud.status = g_host ? g_host->statusText() : (g_client ? g_client->statusText() : "?");

    const nfc::RaceState& r = w->race;
    if (r.phase == nfc::RacePhase::Countdown) {
        const u32 hn = hostNowMs(now);
        const i32 left = static_cast<i32>(r.startAtMs) - static_cast<i32>(hn);
        hud.raceLine = std::string("event ") + std::to_string(r.eventId) + "  countdown " +
                       std::to_string(left > 0 ? (left + 999) / 1000 : 0) + "s";
    } else if (r.phase == nfc::RacePhase::Racing || r.phase == nfc::RacePhase::Finished) {
        hud.raceLine = std::string("event ") + std::to_string(r.eventId) + "  " + nfc::toString(r.phase) +
                       "  " + std::to_string(r.finisherCount) + "/" + std::to_string(w->connectedCount()) + " finished";
    } else {
        hud.raceLine = "no race - F6 opens event " + std::to_string(g_cfg.eventId);
    }

    for (int i = 0; i < nfc::kMaxPlayers; ++i) {
        if (!r.p[i].joined) continue;
        const nfc::PlayerInfo& p = w->players[i];
        std::string line = r.p[i].place ? std::to_string(r.p[i].place) + ". " : "-  ";
        line += p.name;
        if (r.p[i].finished)
            line += "  " + formatTime(r.p[i].finishMs);
        else if (r.p[i].dnf)
            line += "  DNF";
        else
            line += "  cp " + std::to_string(r.p[i].checkpoint) + "/" + std::to_string(r.checkpointCount);
        hud.standings.push_back(line);
    }
    // Direction and distance to every other player, straight from the synced
    // car positions: the simplest thing that makes co-op usable before the
    // remote cars are drawn in the world.
    if (w->localSlot != nfc::kNoPlayer) {
        const nfc::Entity* mine = w->find(w->players[w->localSlot].carId);
        if (mine) {
            static const char* dirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
            for (int i = 0; i < nfc::kMaxPlayers; ++i) {
                if (i == w->localSlot || !w->players[i].connected) continue;
                const nfc::Entity* other = w->find(w->players[i].carId);
                if (!other) continue;
                const float dx = other->st.pos.x - mine->st.pos.x;
                const float dz = other->st.pos.z - mine->st.pos.z;
                const float d = std::sqrt(dx * dx + dz * dz);
                const int b = static_cast<int>(std::floor((std::atan2(dx, dz) + 3.14159265f) / (3.14159265f / 4.0f) + 0.5f)) & 7;
                char line[128];
                std::snprintf(line, sizeof(line), "%s  %d m %s", w->players[i].name.c_str(), static_cast<int>(d), dirs[b]);
                hud.remotes.push_back(line);
            }
        }
    }
    const std::string list = w->playerList();
    std::string cur;
    for (char c : list) {
        if (c == '\n') {
            hud.players.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    hud.careerLine = "career: " + std::to_string(w->progress.revision) + " events | cash " +
                     std::to_string(w->progress.cash) + " | rep " + std::to_string(w->progress.rep);

    if (!hud.bound) hud.warnings.push_back("car/traffic binding missing: see docs/GAME-BINDINGS.md");
    if (g_client && !g_client->connected()) hud.warnings.push_back("not connected to a host");
    if (g_game) {
        const auto& notes = g_game->notices();
        const int start = notes.size() > 5 ? static_cast<int>(notes.size()) - 5 : 0;
        for (int i = start; i < static_cast<int>(notes.size()); ++i) hud.notices.push_back(notes[i]);
    }
    return hud;
}

void updateRenderTargets() {
    nfc::CarRenderer& renderer = nfc::CarRenderer::instance();
    if (!renderer.ready()) return;
    std::vector<nfc::PuppetTarget> puppets;
    nfc::World* w = world();
    if (w && w->localSlot != nfc::kNoPlayer && g_game) {
        int slot = 0;
        for (int i = 0; i < nfc::kMaxPlayers; ++i) {
            if (i == w->localSlot || !w->players[i].connected) continue;
            const nfc::Entity* e = w->find(w->players[i].carId);
            if (!e || !e->active) continue;
            const u32 car = g_game->carAt(1 + slot);  // entry 0 is the local car
            if (car) {
                nfc::PuppetTarget t;
                t.carPointer = car;
                t.pos = e->st.pos;
                t.yaw = e->st.yaw;
                puppets.push_back(t);
            }
            ++slot;
            if (slot >= 3) break;
        }
    }
    // Visual smoke test: put the borrowed car right in front of the local car,
    // so a screenshot proves the render path without driving to the other player.
    if (std::getenv("NFSU2_COOP_PUPPET_FRONT") && !puppets.empty() && w && w->localSlot != nfc::kNoPlayer) {
        const nfc::Entity* me = w->find(w->players[w->localSlot].carId);
        if (me) {
            puppets[0].pos.x = me->st.pos.x + std::sin(me->st.yaw) * 7.0f;
            puppets[0].pos.y = me->st.pos.y + std::cos(me->st.yaw) * 7.0f;
            puppets[0].pos.z = me->st.pos.z;
            puppets[0].yaw = me->st.yaw;
        }
    }
    renderer.setTargets(g_game ? g_game->carPointer() : 0, puppets);
}

void tickOnce(u32 now) {
    if (g_host) g_host->tick(now);
    if (g_client) g_client->tick(now);
    updateRenderTargets();

    if (keyPressed(VK_F5)) {
        if (g_host)
            g_host->forceStart();
        else if (g_client)
            g_client->setReady(true);
    }
    if (keyPressed(VK_F6) && g_host) {
        nfc::World* w = world();
        if (!w || w->race.phase == nfc::RacePhase::Idle || w->race.phase == nfc::RacePhase::Finished)
            g_host->openRace(g_cfg.eventId, 4001, 10);
    }
    if (keyPressed(VK_F7) && g_client)
        g_client->reportCareerEvent(nfc::careerKey(nfc::kCareerRace, g_cfg.eventId));
    if (keyPressed(VK_F8)) nfc::Overlay::instance().toggle();
    if (keyPressed(VK_F9)) {
        if (g_host)
            g_host->resync(nfc::kNoPlayer);
        else if (g_client)
            g_client->requestResync();
    }
}

DWORD WINAPI tickThread(LPVOID) {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(g_module, exePath, MAX_PATH);
    std::string dir = exePath;
    const std::size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir.resize(slash + 1);
    g_iniPath = dir + "nfsu2coop.ini";
    const std::string logPath = dir + "NFSU2Coop-" + std::to_string(GetCurrentProcessId()) + ".log";

    FILE* probe = std::fopen(g_iniPath.c_str(), "rb");
    if (!probe) {
        if (FILE* f = std::fopen(g_iniPath.c_str(), "wb")) {
            const std::string text = nfc::defaultConfigText();
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    } else {
        std::fclose(probe);
    }
    if (!nfc::loadConfig(g_iniPath, g_cfg)) {
        nfc::logOpen(logPath, nfc::LogLevel::Info, true);
        NFC_ERROR("cannot read %s, using defaults", g_iniPath.c_str());
    }
    g_cfg.iniPath = g_iniPath;
    nfc::logOpen(logPath, g_cfg.verbose ? nfc::LogLevel::Debug : nfc::LogLevel::Info, true);
    NFC_INFO("NFSU2Coop %s starting (module %p)", nfc::versionString(), static_cast<void*>(g_module));

    nfc::Overlay& overlay = nfc::Overlay::instance();
    g_game = std::make_unique<nfc::Nfsu2Game>();
    if (!g_game->init(g_module, g_cfg)) {
        NFC_ERROR("game adapter init failed");
        return 0;
    }
    if (g_cfg.overlay) {
        overlay.init();
        overlay.setGameThreadHook([] { g_game->pumpGameThread(); });
    }
    if (g_cfg.drawRemoteCars && g_game->bound()) {
        if (!nfc::CarRenderer::instance().install(g_game->renderVa(), g_cfg.modelSwap))
            NFC_WARN("renderer: not installed, remote cars will not be drawn");
    }

    nfc::SessionConfig sc;
    sc.hostMode = g_cfg.host;
    sc.port = g_cfg.port;
    sc.serverHost = g_cfg.serverHost;
    sc.playerName = g_cfg.playerName;
    sc.sessionName = g_cfg.sessionName;
    sc.snapshotHz = g_cfg.snapshotHz;
    sc.countdownMs = g_cfg.countdownMs;
    sc.maxTraffic = g_cfg.maxTraffic;
    sc.maxBots = g_cfg.maxBots;
    sc.publishGameEntities = g_cfg.publishTraffic;
    sc.hideLocalTraffic = true;

    if (g_cfg.host) {
        g_host = std::make_unique<nfc::HostSession>(*g_game, sc);
        if (!g_host->start()) {
            // Another instance on this PC already hosts: join it instead of
            // leaving the player offline. Handy for two copies on one machine.
            NFC_WARN("host start failed on port %u, falling back to a client", sc.port);
            g_host.reset();
            sc.hostMode = false;
            sc.serverHost = "127.0.0.1";
            g_client = std::make_unique<nfc::ClientSession>(*g_game, sc);
            if (!g_client->start()) NFC_ERROR("client fallback failed");
            else g_game->onNotice("port busy: joined the local host as a client");
        }
    } else {
        g_client = std::make_unique<nfc::ClientSession>(*g_game, sc);
        if (!g_client->start()) NFC_ERROR("client start failed");
    }

    g_ready = true;
    u32 lastHud = 0;
    while (!g_stop) {
        const u32 now = static_cast<u32>(nfc::nowMs());
        tickOnce(now);
        if (now - lastHud > 100) {
            lastHud = now;
            overlay.setState(buildHud(now));
        }
        nfc::sleepMs(5);
    }

    if (g_host) g_host->stop();
    if (g_client) g_client->stop();
    nfc::CarRenderer::instance().shutdown();
    overlay.shutdown();
    g_game->shutdown();
    nfc::logClose();
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        g_thread = CreateThread(nullptr, 0, tickThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stop, 1);
        if (g_thread) {
            WaitForSingleObject(g_thread, 500);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
    }
    return TRUE;
}








