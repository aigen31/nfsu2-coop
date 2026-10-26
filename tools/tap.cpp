// tap.dll: read-only reconnaissance probe injected into a running speed2.exe.
//
// Hooks CarRenderInfo::Render (0x620AE0) with a generated detour: pushad/pushfd,
// log ecx (this), restore, then jump through a slot holding MinHook's
// trampoline. Registers and the caller's stack reach the original untouched, so
// it does not matter how many stack arguments Render takes.
//
// Build: tools\build-tools.ps1   Inject: build\coop-inject.exe --pid N --dll build\tap.dll
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <MinHook.h>

namespace {

FILE* g_log = nullptr;
std::mutex g_mutex;
std::map<void*, int> g_counts;
void* g_hot = nullptr;

void hexDump(const void* p, std::size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; i += 16) {
        std::fprintf(g_log, "%08llX  ", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p) + i));
        for (std::size_t j = 0; j < 16 && i + j < n; ++j) std::fprintf(g_log, "%02X ", b[i + j]);
        std::fprintf(g_log, "  ");
        for (std::size_t j = 0; j < 16 && i + j < n; ++j) {
            const char c = static_cast<char>(b[i + j]);
            std::fprintf(g_log, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        std::fprintf(g_log, "\n");
    }
}

void floatDump(const void* p, std::size_t n) {
    const float* f = static_cast<const float*>(p);
    for (std::size_t i = 0; i + 4 <= n / 4; i += 4) {
        std::fprintf(g_log, "  +%04X:", static_cast<unsigned>(i * 4));
        for (std::size_t j = 0; j < 4 && i + j < n / 4; ++j) {
            const float v = f[i + j];
            if (v == v && v > -1.0e8f && v < 1.0e8f)
                std::fprintf(g_log, " %12.4f", v);
            else
                std::fprintf(g_log, " %12s", ".");
        }
        std::fprintf(g_log, "\n");
    }
}

std::uintptr_t* g_slot = nullptr;
void* g_detour = nullptr;

}  // namespace

void* g_trampoline = nullptr;

extern "C" void logRender(void* self) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_log) return;
    auto it = g_counts.find(self);
    if (it == g_counts.end()) {
        if (g_counts.size() >= 24) {
            g_counts[self] = 0;
        } else {
            g_counts[self] = 1;
            std::fprintf(g_log, "=== CAR %p ===\n", self);
            hexDump(self, 0x180);
            floatDump(self, 0x180);
            std::fflush(g_log);
            return;
        }
    } else {
        ++it->second;
    }
    g_hot = self;
}

namespace {

void* buildDetour() {
    unsigned char* code =
        static_cast<unsigned char*>(VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return nullptr;
    unsigned char* slot = code + 96;
    std::memset(slot, 0, 16);
    unsigned char* p = code;
    *p++ = 0x60;  // pushad
    *p++ = 0x9C;  // pushfd
    *p++ = 0x51;  // push ecx
    *p++ = 0xE8;  // call rel32
    const std::intptr_t rel = reinterpret_cast<unsigned char*>(reinterpret_cast<void*>(&logRender)) - (p + 4);
    std::memcpy(p, &rel, 4);
    p += 4;
    *p++ = 0x83;
    *p++ = 0xC4;
    *p++ = 0x04;  // add esp, 4
    *p++ = 0x9D;  // popfd
    *p++ = 0x61;  // popad
    *p++ = 0xFF;
    *p++ = 0x25;  // jmp dword ptr [imm32]
    const std::uint32_t slotAddr = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(slot));
    std::memcpy(p, &slotAddr, 4);
    p += 4;
    FlushInstructionCache(GetCurrentProcess(), code, 128);
    g_slot = reinterpret_cast<std::uintptr_t*>(slot);
    return code;
}

DWORD WINAPI initThread(LPVOID) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir = path;
    const std::size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir.resize(slash + 1);
    const std::string logPath = dir + "tap-" + std::to_string(GetCurrentProcessId()) + ".log";
    g_log = std::fopen(logPath.c_str(), "w");
    if (!g_log) return 0;
    std::fprintf(g_log, "tap pid=%lu base=%p\n", GetCurrentProcessId(), GetModuleHandleA(nullptr));

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        std::fprintf(g_log, "MH_Initialize failed\n");
        std::fflush(g_log);
        return 0;
    }
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    void* target = reinterpret_cast<void*>(base + 0x220AE0);  // CarRenderInfo::Render (VA 0x620AE0)
    g_detour = buildDetour();
    if (!g_detour) {
        std::fprintf(g_log, "detour alloc failed\n");
        std::fflush(g_log);
        return 0;
    }
    const MH_STATUS st = MH_CreateHook(target, g_detour, &g_trampoline);
    std::fprintf(g_log, "hook %p status=%d trampoline=%p\n", target, static_cast<int>(st), g_trampoline);
    if (st == MH_OK) {
        if (g_slot) *g_slot = reinterpret_cast<std::uintptr_t>(g_trampoline);
        MH_EnableHook(target);
        std::fprintf(g_log, "hook enabled\n");
    }
    std::fflush(g_log);
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) {
            std::fclose(g_log);
            g_log = nullptr;
        }
    }
    return TRUE;
}
