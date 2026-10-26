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
    bool valid = false;
    f32 pos[3] = {};
    f32 pos2[3] = {};
    f32 basis[12] = {};
};
SavedCar g_saved[kMaxPuppets];
// Model identity lives in a few dwords near the start of the car object:
// +0x0C is the model name ("CELICA", "3000GT"), +0x10/+0x14/+0x20 point at the
// per-model parameter tables. Copying them lets a borrowed traffic car wear the
// player's model for one frame.
constexpr u32 kModelFields[] = {0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x2C};
constexpr int kModelFieldCount = 8;
u32 g_savedModel[kMaxPuppets][kModelFieldCount] = {};
bool g_modelSwap = false;
bool g_modelSwapCfg = false;
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

void applyPuppet(int slot, const PuppetTarget& t) {
    SavedCar& s = g_saved[slot];
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

    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (g_localCar && carObj == g_localCar) return;  // never touch our own car
    for (int i = 0; i < static_cast<int>(g_puppets.size()) && i < kMaxPuppets; ++i) {
        if (g_puppets[i].carPointer != carObj) continue;
        if (g_saved[i].valid) return;
        applyPuppet(i, g_puppets[i]);
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

void CarRenderer::restorePuppets() {
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
        s.valid = false;
    }
}

void CarRenderer::clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_puppets.clear();
}

}  // namespace nfc


