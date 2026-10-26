#include "nfsu2_bindings.h"

#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace nfc {

bool Nfsu2Game::safeRead(const void* addr, void* out, std::size_t n) {
    if (!addr || !out || n == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(addr);
    if (a < base) return false;
    const std::size_t avail = static_cast<std::size_t>(mbi.RegionSize) - (a - base);
    if (n > avail) return false;
    std::memcpy(out, addr, n);
    return true;
}

u8* Nfsu2Game::at(u32 va) const {
    if (!module_ || va < image_.imageBase()) return nullptr;
    return reinterpret_cast<u8*>(reinterpret_cast<std::uintptr_t>(module_) + (va - image_.imageBase()));
}

bool Nfsu2Game::init(HMODULE module, const GameConfig& cfg) {
    (void)module;
    cfg_ = cfg;
    // The game is the main module, not this .asi. Anchors and pointers are
    // relative to speed2.exe's image base.
    module_ = GetModuleHandleA(nullptr);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module_, path, MAX_PATH);
    modulePath_ = path;
    if (!image_.loadModule(module_)) {
        NFC_ERROR("game: cannot parse speed2.exe");
        return false;
    }
    NFC_INFO("game: %s base=%p imageBase=0x%X", modulePath_.c_str(), static_cast<void*>(module_),
             image_.imageBase());

    GameBindings tmp;
    if (!cfg.iniPath.empty()) loadBindings(cfg.iniPath, b_);
    if (!b_.carBound() && image_.sizeOfImage() == 0x532000 && image_.entryPoint() == 0x0035BCC7) {
        // Verified build (speed2.exe 4,800,512 bytes, sha256 bc34932b...): the
        // local car is entry 0 of the car array at 0x89CCF8, position +0x50.
        b_.carArray = 0x89CCF8;
        b_.carIndex = 0;
        b_.carStride = 4;
        b_.posOffset = 0x50;
        b_.rotOffset = 0x90;
        NFC_INFO("game: using built-in bindings for the verified build");
    }
    if (!b_.carBound()) {
        note("game memory not bound: car/traffic sync off (see docs/GAME-BINDINGS.md)");
        NFC_WARN("game: no car bindings, protocol-only mode");
    } else {
        NFC_INFO("game: bindings carArray=%08X index=%u pos=%X traffic=%08X", b_.carArray, b_.carIndex,
                 b_.posOffset, b_.trafficList);
    }
    // Prove the documented anchors resolve in this exact build.
    int ok = 0;
    for (const AnchorCheck& c : checkAnchors(image_)) {
        if (c.inText) ++ok;
        NFC_DEBUG("anchor %-30s %s %s", c.name.c_str(), formatVa(c.va).c_str(), c.inText ? "ok" : "MISSING");
    }
    NFC_INFO("game: %d anchors resolved", ok);
    return true;
}

void Nfsu2Game::shutdown() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    pending_.clear();
}

bool Nfsu2Game::readLocalCar(CarState& out) {
    if (!b_.carBound()) return false;
    u8* car = nullptr;
    if (b_.carArray) {
        const u32 slot = b_.carArray + b_.carIndex * b_.carStride;
        if (!safeRead(at(slot), &car, sizeof(car)) || !car) return false;
    } else {
        if (!safeRead(at(b_.carPointer), &car, sizeof(car)) || !car) return false;
    }
    CarState st;
    if (!safeRead(car + b_.posOffset, &st.pos, sizeof(Vec3))) return false;
    if (b_.velOffset) safeRead(car + b_.velOffset, &st.vel, sizeof(Vec3));
    if (b_.yawOffset) safeRead(car + b_.yawOffset, &st.yaw, sizeof(f32));
    if (b_.rotOffset) {
        // 3x3 basis as three vec4 rows; row 1 is the car's forward axis.
        f32 m[12] = {};
        if (safeRead(car + b_.rotOffset, m, sizeof(m))) {
            st.yaw = std::atan2(m[4], m[5]);
            st.pitch = std::asin(clampf(m[6], -1.0f, 1.0f));
        }
    }
    if (b_.speedOffset) {
        f32 speed = 0;
        if (safeRead(car + b_.speedOffset, &speed, sizeof(speed))) st.vel = {speed, 0, 0};
    }
    if (b_.gearOffset) {
        u16 gear = 1;
        if (safeRead(car + b_.gearOffset, &gear, sizeof(gear))) st.gear = gear;
    }
    if (!st.sane()) return false;
    lastCarPtr_ = static_cast<u32>(reinterpret_cast<std::uintptr_t>(car));
    out = st;
    return true;
}

u32 Nfsu2Game::carModel() const {
    if (!b_.carBound()) return 0;
    const u32 car = carAt(b_.carIndex);
    if (!car) return 0;
    u32 namePtr = 0;
    char name[32] = {};
    if (!safeRead(at(car + 0x0C), &namePtr, sizeof(namePtr)) || !namePtr) return 0;
    if (!safeRead(at(namePtr), name, sizeof(name) - 1)) return 0;
    // The model table is a fixed array at 0x890170 with a 0x940 stride on this build;
    // the index of each model was read out of the game (see docs/GAME-BINDINGS.ru.md).
    struct ModelEntry {
        const char* name;
        u32 id;
    };
    static const ModelEntry kModels[] = {
        {"CELICA", 0x00890170},   {"MIATA", 0x00890AB0},     {"GOLF", 0x008913F0},     {"CIVIC", 0x00891D30},
        {"TAXI", 0x00892670},     {"4DR_SEDAN", 0x00892FB0}, {"SUV", 0x00894230},      {"PICKUP", 0x00894B70},
        {"HATCHBACK02", 0x008954B0}, {"4DR_SEDAN02", 0x00896730}, {"HATCHBACK", 0x00897070},
    };
    for (const ModelEntry& m : kModels)
        if (std::strcmp(name, m.name) == 0) return m.id;
    return 0;
}

u32 Nfsu2Game::carPaint() const { return 0; }  // paint fields not located yet

u32 Nfsu2Game::carAt(u32 index) const {
    if (!b_.carArray) return 0;
    u32 car = 0;
    const u32 slot = b_.carArray + index * b_.carStride;
    if (!safeRead(at(slot), &car, sizeof(car))) return 0;
    return car;
}

u32 Nfsu2Game::renderVa() const { return static_cast<u32>(reinterpret_cast<std::uintptr_t>(module_)) + (0x620AE0u - 0x400000u); }

u32 Nfsu2Game::zoneId() const {
    if (!b_.zonePointer) return 1;
    u32 zone = 1;
    safeRead(at(b_.zonePointer), &zone, sizeof(zone));
    return zone;
}

void Nfsu2Game::readGameEntities(std::vector<GameEntityView>& out) {
    out.clear();
    traffic_ = 0;
    if (!b_.trafficBound()) return;
    u8* list = nullptr;
    if (!safeRead(at(b_.trafficList), &list, sizeof(list)) || !list) return;
    int count = 0;
    if (!safeRead(at(b_.trafficCount), &count, sizeof(count))) return;
    count = clampi(count, 0, 256);
    for (int i = 0; i < count; ++i) {
        u8* e = list + static_cast<std::size_t>(i) * b_.trafficStride;
        GameEntityView v;
        if (!safeRead(e + b_.trafficPosOffset, &v.st.pos, sizeof(Vec3))) continue;
        if (b_.trafficModelOffset) safeRead(e + b_.trafficModelOffset, &v.model, sizeof(u8));
        v.kind = EKind::Traffic;
        v.handle = static_cast<u32>(reinterpret_cast<std::uintptr_t>(e));
        if (!v.st.sane()) continue;
        out.push_back(v);
        ++traffic_;
    }
}

void Nfsu2Game::onSpawn(EntityId id, EKind kind, u8 model, const CarState& st) {
    (void)st;
    puppets_[id] = kind;
    (void)model;
}

void Nfsu2Game::onUpdate(EntityId id, EKind kind, const CarState& st) {
    (void)st;
    puppets_[id] = kind;
}

void Nfsu2Game::onDespawn(EntityId id, EKind kind) {
    (void)kind;
    puppets_.erase(id);
}

void Nfsu2Game::queue(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    pending_.push_back(std::move(fn));
}

void Nfsu2Game::onRace(const RaceState& race, RacePhase previous) {
    (void)previous;
    race_ = race;
    if (race.phase == RacePhase::Countdown && race.eventId != lastStartedEvent_) {
        lastStartedEvent_ = race.eventId;
        if (b_.startEventFn) {
            const u32 fn = b_.startEventFn;
            const u16 event = race.eventId;
            queue([this, fn, event] {
                using StartFn = void(__cdecl*)(int);
                auto f = reinterpret_cast<StartFn>(at(fn));
                NFC_INFO("game: StartEvent(%u)", event);
                if (f) f(static_cast<int>(event));
            });
            note("starting event " + std::to_string(race.eventId) + " in game");
        } else if (!warnedStart_) {
            warnedStart_ = true;
            note("start event " + std::to_string(race.eventId) + " yourself; standings sync");
        }
    }
}

void Nfsu2Game::onCareer(const ProgressLedger& ledger) {
    ledger_ = ledger;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "career: %u events, cash %u, rep %u", ledger.revision, ledger.cash, ledger.rep);
    note(buf);
    if (b_.careerCashFn) {
        const u32 fn = b_.careerCashFn;
        const u32 cash = ledger.cash;
        queue([this, fn, cash] {
            using SetFn = void(__cdecl*)(int);
            auto f = reinterpret_cast<SetFn>(at(fn));
            if (f) f(static_cast<int>(cash));
        });
    }
    if (b_.careerRepFn) {
        const u32 fn = b_.careerRepFn;
        const u32 rep = ledger.rep;
        queue([this, fn, rep] {
            using SetFn = void(__cdecl*)(int);
            auto f = reinterpret_cast<SetFn>(at(fn));
            if (f) f(static_cast<int>(rep));
        });
    }
}

void Nfsu2Game::onNotice(const std::string& text) {
    note(text);
}

void Nfsu2Game::onConnection(bool connected, const std::string& reason) {
    NFC_INFO("net: %s %s", connected ? "connected" : "disconnected", reason.c_str());
    note(connected ? "connected: " + reason : "disconnected: " + reason);
}

void Nfsu2Game::note(const std::string& text) {
    notices_.push_back(text);
    if (notices_.size() > 24) notices_.erase(notices_.begin());
    NFC_INFO("notice: %s", text.c_str());
}

void Nfsu2Game::pumpGameThread() {
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        work.swap(pending_);
    }
    for (auto& fn : work) fn();
}

}  // namespace nfc





