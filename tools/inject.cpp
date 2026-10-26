// coop-inject: load a DLL into a running process (same bitness).
//
//   coop-inject --pid 1234 --dll "C:\path\tap.dll"
//
// Used by the RE tools only; the shipped mod does not inject anything.
#include <windows.h>
#include <cstdint>

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    int pid = 0;
    std::string dll;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--pid") && i + 1 < argc) pid = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
    }
    if (!pid || dll.empty()) {
        std::printf("usage: coop-inject --pid <pid> --dll <path>\n");
        return 1;
    }
    char full[MAX_PATH] = {};
    if (!GetFullPathNameA(dll.c_str(), MAX_PATH, full, nullptr)) {
        std::printf("bad dll path\n");
        return 1;
    }
    std::printf("dll: %s\n", full);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                  PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        std::printf("OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    const std::size_t len = std::strlen(full) + 1;
    void* remote = VirtualAllocEx(proc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        std::printf("VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }
    if (!WriteProcessMemory(proc, remote, full, len, nullptr)) {
        std::printf("WriteProcessMemory failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(k32, "LoadLibraryA")));
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        std::printf("CreateRemoteThread failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }
    WaitForSingleObject(thread, 5000);
    DWORD code = 0;
    GetExitCodeThread(thread, &code);
    std::printf("LoadLibraryA returned %p (exit %lu)\n", reinterpret_cast<void*>(static_cast<std::uintptr_t>(code)),
                code);
    CloseHandle(thread);
    CloseHandle(proc);
    return code ? 0 : 2;
}

