// recon: read-only memory reconnaissance for the co-op binding table.
//
//   recon --pid <speed2.exe pid> [--scan-data] [--dump <va> <bytes>] [--float <va> <count>]
//
// It never writes to the game. Used to find the local Player/Car objects by the
// invariants documented in docs/GAME-BINDINGS.ru.md (Player+0x58 = state 1..7,
// Player+0x4 = Car, Car+0x958 = CarDamage with a .rdata vtable).
#include <windows.h>
#include <cstdint>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;

HANDLE g_proc = nullptr;

bool rd(u64 va, void* out, std::size_t n) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(g_proc, reinterpret_cast<LPCVOID>(va), out, n, &got)) return false;
    return got == n;
}

u32 rd32(u64 va) {
    u32 v = 0;
    rd(va, &v, sizeof(v));
    return v;
}

float rdf(u64 va) {
    float f = 0;
    rd(va, &f, sizeof(f));
    return f;
}

constexpr u64 kImageBase = 0x400000;
constexpr u64 kDataStart = kImageBase + 0x3E8000;
constexpr u64 kDataSize = 0xD35F8;
constexpr u64 kRdataStart = kImageBase + 0x383000;
constexpr u64 kRdataEnd = kImageBase + 0x383000 + 0x641DB;

std::vector<uint32_t> moduleRanges(u64 va) {
    std::vector<uint32_t> out;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQueryEx(g_proc, reinterpret_cast<LPCVOID>(va), &mbi, sizeof(mbi))) return out;
    out.push_back(static_cast<uint32_t>(mbi.BaseAddress ? reinterpret_cast<u64>(mbi.BaseAddress) : 0));
    return out;
}

void dumpFloats(u64 base, int count) {
    std::printf("    floats @%08llX:", static_cast<unsigned long long>(base));
    for (int i = 0; i < count; ++i) {
        if (i % 4 == 0) std::printf("\n      +%03X:", i * 4);
        const float f = rdf(base + i * 4);
        if (std::isfinite(f) && std::fabs(f) < 1.0e7f)
            std::printf(" %10.3f", f);
        else
            std::printf(" %10s", ".");
    }
    std::printf("\n");
}

// A world position: three floats, |x|/|z| sane, y small, not all zero.
bool plausibleTriple(u64 va, float* out) {
    float v[3];
    for (int i = 0; i < 3; ++i) {
        v[i] = rdf(va + i * 4);
        if (!std::isfinite(v[i])) return false;
    }
    if (std::fabs(v[0]) < 1.0f && std::fabs(v[2]) < 1.0f) return false;
    if (std::fabs(v[0]) > 30000.0f || std::fabs(v[2]) > 30000.0f) return false;
    if (v[1] < -500.0f || v[1] > 2000.0f) return false;
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
    return true;
}

void scanData() {
    std::vector<u8> buf(static_cast<std::size_t>(kDataSize));
    if (!rd(kDataStart, buf.data(), buf.size())) {
        std::printf("cannot read .data\n");
        return;
    }
    int found = 0;
    for (std::size_t off = 0; off + 4 <= buf.size(); off += 4) {
        u32 p = 0;
        std::memcpy(&p, &buf[off], 4);
        if (p < 0x10000 || p > 0x7FFFFFFF) continue;
        const u32 state = rd32(p + 0x58);
        if (state < 1 || state > 8) continue;
        const u32 car = rd32(p + 4);
        if (car < 0x10000 || car > 0x7FFFFFFF) continue;
        const u32 dmg = rd32(car + 0x958);
        if (dmg < 0x10000 || dmg > 0x7FFFFFFF) continue;
        const u32 vt = rd32(dmg);
        const bool vtOk = vt >= kRdataStart && vt < kRdataEnd;
        std::printf("cand global=%08X P=%08X state=%u car=%08X dmg=%08X vt=%08X%s\n",
                    static_cast<unsigned>(kDataStart + off), p, state, car, dmg, vt, vtOk ? "  VT-OK" : "");
        if (vtOk) {
            dumpFloats(p, 8);
            dumpFloats(car, 12);
        }
        if (++found >= 40) break;
    }
    if (!found) std::printf("no candidates in .data\n");
}

struct Region {
    u64 base = 0;
    SIZE_T size = 0;
};

std::vector<Region> heapRegions() {
    std::vector<Region> out;
    u64 addr = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (VirtualQueryEx(g_proc, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(addr)), &mbi, sizeof(mbi))) {
        const u64 base = reinterpret_cast<u64>(mbi.BaseAddress);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.RegionSize <= (64u << 20) &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE ||
             mbi.Protect == PAGE_WRITECOPY)) {
            out.push_back({base, mbi.RegionSize});
        }
        const u64 next = base + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    return out;
}

std::vector<u64> findBytes(const std::string& needle) {
    std::vector<u64> hits;
    std::vector<u8> buf;
    for (const Region& r : heapRegions()) {
        buf.resize(static_cast<std::size_t>(r.size));
        if (!rd(r.base, buf.data(), buf.size())) continue;
        for (std::size_t i = 0; i + needle.size() <= buf.size(); ++i) {
            if (buf[i] == static_cast<u8>(needle[0]) &&
                std::memcmp(&buf[i], needle.data(), needle.size()) == 0) {
                hits.push_back(r.base + i);
                if (hits.size() >= 12) return hits;
            }
        }
    }
    return hits;
}

void findModel(const std::string& name) {
    const std::string needle = name + '\0';
    const std::vector<u64> strings = findBytes(needle);
    std::printf("model '%s': %zu string(s) in heap\n", name.c_str(), strings.size());
    for (u64 s : strings) std::printf("  string @%08llX\n", static_cast<unsigned long long>(s));
    if (strings.empty()) return;
    std::vector<u32> targets;
    for (u64 s : strings) targets.push_back(static_cast<u32>(s));
    int shown = 0;
    for (const Region& r : heapRegions()) {
        std::vector<u8> buf(static_cast<std::size_t>(r.size));
        if (!rd(r.base, buf.data(), buf.size())) continue;
        for (std::size_t i = 0; i + 4 <= buf.size(); i += 4) {
            u32 v = 0;
            std::memcpy(&v, &buf[i], 4);
            bool match = false;
            for (u32 t : targets) match = match || (v == t);
            if (!match) continue;
            const u64 ref = r.base + i;
            const u64 object = ref - 8;  // CarRenderInfo keeps the model name at +8
            std::printf("referrer @%08llX -> object %08llX\n", static_cast<unsigned long long>(ref),
                        static_cast<unsigned long long>(object));
            dumpFloats(object, 40);
            if (++shown >= 8) return;
        }
    }
}}  // namespace

int main(int argc, char** argv) {
    int pid = 0;
    bool scan = false;
    std::string model;
    u64 dumpVa = 0, floatVa = 0;
    int amount = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--pid") && i + 1 < argc) pid = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--scan-data")) scan = true;
        else if (!std::strcmp(a, "--model") && i + 1 < argc) model = argv[++i];
        else if (!std::strcmp(a, "--dump") && i + 2 < argc) {
            dumpVa = std::strtoull(argv[++i], nullptr, 16);
            amount = std::atoi(argv[++i]);
        } else if (!std::strcmp(a, "--float") && i + 2 < argc) {
            floatVa = std::strtoull(argv[++i], nullptr, 16);
            amount = std::atoi(argv[++i]);
        }
    }
    if (!pid) {
        std::printf("usage: recon --pid <pid> [--scan-data] [--dump <hexva> <bytes>] [--float <hexva> <count>]\n");
        return 1;
    }
    g_proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!g_proc) {
        std::printf("OpenProcess(%d) failed: %lu (run as the same user, 32-bit tool)\n", pid, GetLastError());
        return 1;
    }
    std::printf("pid %d\n", pid);
    if (!model.empty()) findModel(model);
    if (scan) scanData();
    if (dumpVa && amount) {
        std::vector<u8> b(static_cast<std::size_t>(amount));
        if (rd(dumpVa, b.data(), b.size())) {
            for (int i = 0; i < amount; i += 16) {
                std::printf("%08llX  ", static_cast<unsigned long long>(dumpVa + i));
                for (int j = 0; j < 16 && i + j < amount; ++j) std::printf("%02X ", b[i + j]);
                std::printf("  ");
                for (int j = 0; j < 16 && i + j < amount; ++j) {
                    const char c = static_cast<char>(b[i + j]);
                    std::printf("%c", (c >= 32 && c < 127) ? c : '.');
                }
                std::printf("\n");
            }
        } else {
            std::printf("cannot read %08llX\n", static_cast<unsigned long long>(dumpVa));
        }
    }
    if (floatVa && amount) dumpFloats(floatVa, amount);
    CloseHandle(g_proc);
    return 0;
}



