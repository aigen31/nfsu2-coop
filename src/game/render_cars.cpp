#include "render_cars.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <MinHook.h>

#include "core/log.h"

namespace nfc {
namespace {

using u8 = std::uint8_t;

constexpr int kMaxPuppets = 3;

volatile std::uintptr_t g_jump = 0;  // trampoline, written after MH_CreateHook
void* g_trampoline = nullptr;
void* g_detour = nullptr;

std::mutex g_mutex;
u32 g_localCar = 0;
std::vector<PuppetTarget> g_puppets;

struct SavedCar {
    u32 car = 0;
    u32 ri = 0;  // CarRenderInfo seen for this car this frame
    u32 riCar = 0;  // original [ri] (the car object pointer) when cloned
    bool cloned = false;
    bool valid = false;
    f32 pos[3] = {};
    f32 pos2[3] = {};
    f32 basis[12] = {};
};
SavedCar g_saved[kMaxPuppets];

// Render-info experiment: the CarRenderInfo (Render's `this`) is where the
// engine keeps the model/geometry it is about to draw. NFSU2_COOP_DUMP=1 writes
// the local and one borrowed CarRenderInfo to render-dump.txt;
// NFSU2_COOP_SWAP=0x10,0x14,... copies those dword offsets from the local one
// into the borrowed one for the frame.
u32 g_localRI = 0;
bool g_dumpWanted = false;
bool g_dumpedLocal = false;
bool g_dumpedPuppet = false;
std::vector<u32> g_swapOffsets;
struct RISave {
    u32 off = 0;
    u32 val = 0;
};
std::vector<RISave> g_riSaved[kMaxPuppets];
// Model identity lives in a few dwords near the start of the car object:
// +0x0C is the model name ("CELICA", "3000GT"), +0x10/+0x14/+0x20 point at the
// per-model parameter tables. Copying them lets a borrowed traffic car wear the
// player's model for one frame.
constexpr u32 kModelFields[] = {0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x2C};
constexpr int kModelFieldCount = 9;
u32 g_savedModel[kMaxPuppets][kModelFieldCount] = {};

bool g_modelSwap = false;
bool g_modelSwapCfg = false;
bool g_cloneMode = false;
u32 g_clone = 0;  // our copy of the local car object
volatile long g_renderCalls = 0;

void write32(u8*& p, std::uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
void write8(u8*& p, u8 v) { *p++ = v; }

void onRender(void* self);

void* buildDetour() {
    u8* code = static_cast<u8*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return nullptr;
    u8* p = code;
    write8(p, 0x60);  // pushad
    write8(p, 0x9C);  // pushfd
    write8(p, 0x51);  // push ecx (this)
    write8(p, 0xE8);  // call rel32
    {
        const std::intptr_t rel = reinterpret_cast<u8*>(reinterpret_cast<void*>(&onRender)) - (p + 4);
        std::memcpy(p, &rel, 4);
        p += 4;
    }
    write8(p, 0x83);
    write8(p, 0xC4);
    write8(p, 0x04);  // add esp, 4
    write8(p, 0x9D);  // popfd
    write8(p, 0x61);  // popad
    write8(p, 0xFF);  // jmp dword ptr [imm32]
    write8(p, 0x25);
    write32(p, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_jump)));
    FlushInstructionCache(GetCurrentProcess(), code, 64);
    return code;
}

bool readMem(u32 addr, void* out, std::size_t n) {
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(addr)), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(addr)), out, n,
                             nullptr) != FALSE;
}

bool writeMem(u32 addr, const void* in, std::size_t n) {
    if (!addr) return false;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(addr)), in, n,
                              nullptr) != FALSE;
}

void dumpRI(const char* tag, u32 ri) {
    if (!ri) return;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p = path;
    const std::size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos) p.resize(slash + 1);
    p += "render-dump.txt";
    FILE* f = std::fopen(p.c_str(), "ab");
    if (!f) return;
    std::fprintf(f, "%s %08X\n", tag, ri);
    for (u32 off = 0; off < 0x2D80; off += 32) {
        u32 v[8] = {};
        if (!readMem(ri + off, v, sizeof(v))) break;
        std::fprintf(f, "%04X:", off);
        for (int i = 0; i < 8; ++i) std::fprintf(f, " %08X", v[i]);
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

void applyPuppet(int slot, const PuppetTarget& t, u32 ri) {
    SavedCar& s = g_saved[slot];
    s.ri = ri;
    s.cloned = false;
    if (g_cloneMode && g_localCar && g_clone) {
        // Point the game at OUR copy of the local car object: same model and
        // materials as the player, different position, and the live car object
        // is never touched. The heap block may be smaller than 0x1000, so copy
        // only what VirtualQuery says is committed.
        SIZE_T n = 0;
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(g_localCar)), &mbi, sizeof(mbi)) &&
                mbi.State == MEM_COMMIT && mbi.RegionSize > 0) {
                const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                const std::uintptr_t at = static_cast<std::uintptr_t>(g_localCar);
                const std::size_t avail = static_cast<std::size_t>(mbi.RegionSize - (at - base));
                n = avail < 0x1000 ? avail : 0x1000;
            }
        }
        if (n && readMem(g_localCar, reinterpret_cast<void*>(static_cast<std::uintptr_t>(g_clone)), n)) {
            f32 pos[3] = {t.pos.x, t.pos.y, t.pos.z};
            const f32 c = std::cos(t.yaw), sn = std::sin(t.yaw);
            f32 basis[12] = {c, -sn, 0, 0, sn, c, 0, 0, 0, 0, 1, 0};
            writeMem(g_clone + 0x50, pos, sizeof(pos));
            writeMem(g_clone + 0x60, pos, sizeof(pos));
            writeMem(g_clone + 0x90, basis, sizeof(basis));
            if (readMem(ri, &s.riCar, sizeof(u32))) {
                u32 clone = g_clone;
                if (writeMem(ri, &clone, sizeof(clone))) s.cloned = true;
            }
        }
    }
    g_riSaved[slot].clear();
    if (!g_swapOffsets.empty() && g_localRI && ri) {
        for (u32 off : g_swapOffsets) {
            u32 lv = 0, bv = 0;
            if (!readMem(g_localRI + off, &lv, sizeof(lv))) continue;
            if (!readMem(ri + off, &bv, sizeof(bv))) continue;
            if (lv == bv) continue;
            if (writeMem(ri + off, &lv, sizeof(lv))) g_riSaved[slot].push_back({off, bv});
        }
    }
    if (g_modelSwap && g_localCar) {
        for (int f = 0; f < kModelFieldCount; ++f) {
            u32 src = 0, dst = 0;
            if (!readMem(g_localCar + kModelFields[f], &src, sizeof(src))) continue;
            if (!readMem(t.carPointer + kModelFields[f], &dst, sizeof(dst))) continue;
            g_savedModel[slot][f] = dst;
            writeMem(t.carPointer + kModelFields[f], &src, sizeof(src));
        }
    }
    if (!readMem(t.carPointer + 0x50, s.pos, sizeof(s.pos))) return;
    if (!readMem(t.carPointer + 0x60, s.pos2, sizeof(s.pos2))) return;
    if (!readMem(t.carPointer + 0x90, s.basis, sizeof(s.basis))) return;
    s.car = t.carPointer;
    s.valid = true;

    const f32 dx = s.pos2[0] - s.pos[0], dy = s.pos2[1] - s.pos[1], dz = s.pos2[2] - s.pos[2];
    f32 np[3] = {t.pos.x, t.pos.y, t.pos.z};
    f32 np2[3] = {np[0] + dx, np[1] + dy, np[2] + dz};
    const f32 c = std::cos(t.yaw), sn = std::sin(t.yaw);
    f32 nb[12] = {c, -sn, 0, 0, sn, c, 0, 0, 0, 0, 1, 0};

    writeMem(t.carPointer + 0x50, np, sizeof(np));
    writeMem(t.carPointer + 0x60, np2, sizeof(np2));
    writeMem(t.carPointer + 0x90, nb, sizeof(nb));
}

void onRender(void* self) {
    InterlockedIncrement(&g_renderCalls);
    if (!self) return;
    u32 carObj = 0;
    if (!readMem(static_cast<u32>(reinterpret_cast<std::uintptr_t>(self)), &carObj, sizeof(carObj))) return;

    const u32 ri = static_cast<u32>(reinterpret_cast<std::uintptr_t>(self));
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (g_localCar && carObj == g_localCar) {
        g_localRI = ri;
        if (g_dumpWanted && !g_dumpedLocal) {
            g_dumpedLocal = true;
            dumpRI("LOCAL", ri);
        }
        return;  // never touch our own car
    }
    for (int i = 0; i < static_cast<int>(g_puppets.size()) && i < kMaxPuppets; ++i) {
        if (g_puppets[i].carPointer != carObj) continue;
        if (g_saved[i].valid) return;
        if (g_dumpWanted && !g_dumpedPuppet) {
            g_dumpedPuppet = true;
            dumpRI("PUPPET", ri);
        }
        applyPuppet(i, g_puppets[i], ri);
        return;
    }
}

}  // namespace

CarRenderer& CarRenderer::instance() {
    static CarRenderer r;
    return r;
}

bool CarRenderer::install(u32 renderVa, bool modelSwap) {
    g_modelSwapCfg = modelSwap;
    if (installed_) return true;
    if (!renderVa) return false;
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return false;
    g_detour = buildDetour();
    if (!g_detour) {
        NFC_ERROR("render: cannot allocate detour");
        return false;
    }
    void* target = reinterpret_cast<void*>(static_cast<std::uintptr_t>(renderVa));
    if (MH_CreateHook(target, g_detour, &g_trampoline) != MH_OK) {
        NFC_ERROR("render: MH_CreateHook failed");
        return false;
    }
    g_jump = reinterpret_cast<std::uintptr_t>(g_trampoline);
    g_modelSwap = g_modelSwapCfg || std::getenv("NFSU2_COOP_MODEL_SWAP") != nullptr;
    g_dumpWanted = std::getenv("NFSU2_COOP_DUMP") != nullptr;
    g_cloneMode = std::getenv("NFSU2_COOP_CLONE") != nullptr;
    if (g_cloneMode && !g_clone) {
        g_clone = static_cast<u32>(reinterpret_cast<std::uintptr_t>(
            VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)));
        NFC_INFO("render: clone mode, buffer %08X", g_clone);
    }
    if (const char* sw = std::getenv("NFSU2_COOP_SWAP")) {
        std::string s(sw);
        std::size_t pos = 0;
        while (pos < s.size()) {
            const std::size_t comma = s.find(',', pos);
            const std::string part = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            g_swapOffsets.push_back(static_cast<u32>(std::strtoul(part.c_str(), nullptr, 16)));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    // No built-in swap: experiments showed the model is bound at spawn, and
    // writing these fields is not safe in every process. Opt in with
    // NFSU2_COOP_SWAP=0x04,0x08 while experimenting.
    if (!g_swapOffsets.empty()) NFC_INFO("render: render-info swap offsets: %zu", g_swapOffsets.size());
    if (MH_EnableHook(target) != MH_OK) {
        NFC_ERROR("render: MH_EnableHook failed");
        return false;
    }
    installed_ = true;
    NFC_INFO("render: CarRenderInfo::Render hooked at %08X, trampoline %p", renderVa, g_trampoline);
    return true;
}

void CarRenderer::shutdown() {
    if (!installed_) return;
    restorePuppets();
    MH_DisableHook(MH_ALL_HOOKS);
    installed_ = false;
}

void CarRenderer::setTargets(u32 localCarPointer, const std::vector<PuppetTarget>& puppets) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_localCar = localCarPointer;
        g_puppets = puppets;
        if (g_puppets.size() > kMaxPuppets) g_puppets.resize(kMaxPuppets);
    }
    static u32 loggedLocal = 0xFFFFFFFFu;
    static std::size_t loggedCount = 0xFFFFFFFFu;
    if (localCarPointer != loggedLocal || puppets.size() != loggedCount) {
        loggedLocal = localCarPointer;
        loggedCount = puppets.size();
        NFC_INFO("render: localCar=%08X puppets=%zu", localCarPointer, puppets.size());
    }
}

// Model swap that works: FUN_0063A5A0(car, model) mallocs the 0x2EC0-byte render
// object, builds it from the model and refreshes the draw batches. Calling it once
// per borrowed car with the local player's model gives the borrowed car the player's
// body instead of a traffic one.
constexpr std::uintptr_t kReloadModelVa = 0x0063A5A0;
u32 g_reloadedCar[kMaxPuppets] = {};
u32 g_reloadedModel[kMaxPuppets] = {};

void reloadPuppetModel(int slot, u32 car, u32 wantedModel) {
    if (!g_modelSwap || !car) return;
    std::uint32_t model = wantedModel;
    if (!model) {
        if (!g_localCar) return;
        std::uint32_t desc = 0;
        if (!readMem(g_localCar + 0x554, &desc, sizeof(desc)) || !desc) return;
        if (!readMem(desc + 0x04, &model, sizeof(model)) || !model) return;
    }
    if (g_reloadedCar[slot] == car && g_reloadedModel[slot] == model) return;
    using ReloadFn = void(__thiscall*)(void*, std::uint32_t);
    reinterpret_cast<ReloadFn>(kReloadModelVa)(reinterpret_cast<void*>(static_cast<std::uintptr_t>(car)), model);
    g_reloadedCar[slot] = car;
    g_reloadedModel[slot] = model;
    NFC_INFO("render: reloaded model %08X on car %08X (slot %d)", model, car, slot);
}

void CarRenderer::restorePuppets() {
    for (int i = 0; i < static_cast<int>(g_puppets.size()) && i < kMaxPuppets; ++i)
        reloadPuppetModel(i, g_puppets[i].carPointer, g_puppets[i].model);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < kMaxPuppets; ++i) {
        SavedCar& s = g_saved[i];
        if (!s.valid) continue;
        writeMem(s.car + 0x50, s.pos, sizeof(s.pos));
        writeMem(s.car + 0x60, s.pos2, sizeof(s.pos2));
        writeMem(s.car + 0x90, s.basis, sizeof(s.basis));
        if (g_modelSwap) {
            for (int f = 0; f < kModelFieldCount; ++f)
                writeMem(s.car + kModelFields[f], &g_savedModel[i][f], sizeof(u32));
        }
        if (s.cloned) writeMem(s.ri, &s.riCar, sizeof(u32));
        for (const RISave& sv : g_riSaved[i]) writeMem(s.ri + sv.off, &sv.val, sizeof(u32));
        g_riSaved[i].clear();
        s.valid = false;
    }
}

void CarRenderer::clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_puppets.clear();
}

}  // namespace nfc










