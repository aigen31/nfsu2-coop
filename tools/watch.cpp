// watch.dll: hardware write-watchpoints on the car-array slots, no debugger.
//
// A thread-level data breakpoint (Dr0..Dr3) is set on the game's main thread, and
// a vectored exception handler logs the instruction pointer and the return
// addresses on the stack for the first hits. That is how the code that fills the
// car render list (and binds the model) is located.
//
// Build: tools\build-tools.ps1  Inject: coop-inject --pid N --dll watch.dll
#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <string>

namespace {

constexpr std::uint32_t kSlots[4] = {0x89CCF8, 0x89CCFC, 0x89CD00, 0x89CD04};
FILE* g_log = nullptr;
int g_hits = 0;
PVOID g_veh = nullptr;

BOOL CALLBACK enumProc(HWND w, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w)) {
        *reinterpret_cast<HWND*>(lp) = w;
        return FALSE;
    }
    return TRUE;
}

LONG CALLBACK onException(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (g_hits >= 40) return EXCEPTION_CONTINUE_EXECUTION;
    CONTEXT* c = info->ContextRecord;
    ++g_hits;
    if (g_log) {
        std::fprintf(g_log, "HIT %d eip=%08lX dr6=%08lX slots=%08lX %08lX %08lX %08lX\n", g_hits,
                     static_cast<unsigned long>(c->Eip), static_cast<unsigned long>(c->Dr6),
                     *reinterpret_cast<unsigned long*>(kSlots[0]), *reinterpret_cast<unsigned long*>(kSlots[1]),
                     *reinterpret_cast<unsigned long*>(kSlots[2]), *reinterpret_cast<unsigned long*>(kSlots[3]));
        const std::uint32_t* sp = reinterpret_cast<const std::uint32_t*>(c->Esp);
        std::fprintf(g_log, "  stack:");
        for (int i = 0; i < 10; ++i) std::fprintf(g_log, " %08lX", static_cast<unsigned long>(sp[i]));
        std::fprintf(g_log, "\n");
        std::fflush(g_log);
    }
    if (g_hits >= 40) c->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI initThread(LPVOID) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p = path;
    const std::size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos) p.resize(slash + 1);
    p += "watch-" + std::to_string(GetCurrentProcessId()) + ".log";
    g_log = std::fopen(p.c_str(), "w");
    if (!g_log) return 0;
    std::fprintf(g_log, "watch pid=%lu\n", GetCurrentProcessId());

    HWND hwnd = nullptr;
    for (DWORD t = 0; t < 100 && !hwnd; ++t) {
        EnumWindows(enumProc, reinterpret_cast<LPARAM>(&hwnd));
        if (!hwnd) Sleep(100);
    }
    if (!hwnd) {
        std::fprintf(g_log, "no window found\n");
        std::fclose(g_log);
        return 0;
    }
    DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
    std::fprintf(g_log, "hwnd=%p tid=%lu\n", hwnd, tid);
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!th) {
        std::fprintf(g_log, "OpenThread failed %lu\n", GetLastError());
        std::fclose(g_log);
        return 0;
    }
    g_veh = AddVectoredExceptionHandler(1, onException);
    SuspendThread(th);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(th, &c)) {
        c.Dr0 = kSlots[0];
        c.Dr1 = kSlots[1];
        c.Dr2 = kSlots[2];
        c.Dr3 = kSlots[3];
        DWORD dr7 = 0x00000001;  // L0
        for (int i = 0; i < 4; ++i) {
            dr7 |= 1u << (2 * i);       // local enable
            dr7 |= 1u << (16 + 4 * i);  // RW = write
            dr7 |= 3u << (18 + 4 * i);  // LEN = 4 bytes
        }
        c.Dr7 = dr7;
        if (!SetThreadContext(th, &c)) std::fprintf(g_log, "SetThreadContext failed %lu\n", GetLastError());
        else std::fprintf(g_log, "watchpoints armed dr7=%08lX\n", static_cast<unsigned long>(dr7));
    } else {
        std::fprintf(g_log, "GetThreadContext failed %lu\n", GetLastError());
    }
    ResumeThread(th);
    CloseHandle(th);
    std::fflush(g_log);
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
    }
    return TRUE;
}

