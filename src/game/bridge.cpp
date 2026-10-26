#include "bridge.h"

#include <windows.h>

#include <coopbridge/server.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../core/log.h"

namespace nfc {
namespace {

coopbridge::Server g_server;
bool g_running = false;

bool readMem(std::uint32_t va, void* dst, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(va)), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return false;
    std::memcpy(dst, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va)), n);
    return true;
}

bool writeMem(std::uint32_t va, const void* src, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(va)), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(va)), n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(va)), src, n);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(va)), n, old, &ignored);
    return true;
}

std::string toHex(const std::uint8_t* p, std::size_t n) {
    static const char* kDigits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(kDigits[p[i] >> 4]);
        s.push_back(kDigits[p[i] & 0xF]);
    }
    return s;
}

bool fromHex(const std::string& hex, std::vector<std::uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

coopbridge::json::Value slotList() {
    auto arr = coopbridge::json::Value::array();
    for (int i = 0; i < 4; ++i) {
        std::uint32_t p = 0;
        if (readMem(0x89CCF8 + static_cast<std::uint32_t>(i) * 4, &p, sizeof(p))) arr.push(static_cast<std::int64_t>(p));
        else arr.push(coopbridge::json::Value());
    }
    return arr;
}

class Nfsu2Adapter : public coopbridge::Adapter {
public:
    coopbridge::Capabilities capabilities() const override {
        coopbridge::Capabilities c;
        c.state = true;
        return c;
    }

    coopbridge::json::Value State(const std::vector<std::string>&) override {
        auto v = coopbridge::json::Value::object();
        v.set("pid", static_cast<std::int64_t>(GetCurrentProcessId()));
        v.set("cars", slotList());
        std::uint32_t local = 0;
        if (readMem(0x89CCF8, &local, sizeof(local)) && local) {
            float pos[3] = {};
            if (readMem(local + 0x50, pos, sizeof(pos))) {
                auto p = coopbridge::json::Value::array();
                p.push(pos[0]);
                p.push(pos[1]);
                p.push(pos[2]);
                v.set("localPos", p);
            }
            std::uint32_t namePtr = 0;
            if (readMem(local + 0x0C, &namePtr, sizeof(namePtr)) && namePtr) {
                char name[32] = {};
                if (readMem(namePtr, name, sizeof(name) - 1)) v.set("localModel", std::string(name));
            }
        }
        return v;
    }
};

Nfsu2Adapter g_adapter;

}  // namespace

bool Bridge::start(std::uint16_t port) {
    if (g_running || port == 0) return false;
    coopbridge::Options opt;
    opt.game = "nfsu2";
    opt.build = "1.2";
    opt.tcpPort = static_cast<int>(port);
    opt.tcpHost = "127.0.0.1";
    if (!g_server.Start(opt, g_adapter)) {
        NFC_WARN("bridge: start failed on port %u: %s", port, g_server.lastError().c_str());
        return false;
    }
    g_server.Register("mem.read", [](const coopbridge::json::Value& p) {
        const auto addr = static_cast<std::uint32_t>(p.get("addr").asInt());
        std::size_t size = static_cast<std::size_t>(p.get("size").asInt(4));
        if (size == 0 || size > 4096) throw coopbridge::Error(-32010, "size must be 1..4096");
        std::vector<std::uint8_t> buf(size);
        if (!readMem(addr, buf.data(), size)) throw coopbridge::Error(-32011, "unreadable address");
        auto r = coopbridge::json::Value::object();
        r.set("hex", toHex(buf.data(), size));
        return r;
    });
    g_server.Register("mem.write", [](const coopbridge::json::Value& p) {
        const auto addr = static_cast<std::uint32_t>(p.get("addr").asInt());
        std::vector<std::uint8_t> bytes;
        if (!fromHex(p.get("hex").asString(), bytes) || bytes.empty())
            throw coopbridge::Error(-32012, "hex must be an even-length hex string");
        if (!writeMem(addr, bytes.data(), bytes.size())) throw coopbridge::Error(-32011, "unwritable address");
        return coopbridge::json::Value(true);
    });
    g_server.Register("car.slots", [](const coopbridge::json::Value&) { return slotList(); });
    g_server.Register("car.name", [](const coopbridge::json::Value& p) {
        std::uint32_t car = static_cast<std::uint32_t>(p.get("car").asInt());
        if (!car) {
            std::uint32_t slot = static_cast<std::uint32_t>(p.get("slot").asInt());
            if (!readMem(0x89CCF8 + slot * 4, &car, sizeof(car))) car = 0;
        }
        if (!car) throw coopbridge::Error(-32013, "no car in that slot");
        std::uint32_t namePtr = 0;
        if (!readMem(car + 0x0C, &namePtr, sizeof(namePtr)) || !namePtr)
            throw coopbridge::Error(-32014, "car has no model name");
        char name[32] = {};
        if (!readMem(namePtr, name, sizeof(name) - 1)) throw coopbridge::Error(-32011, "unreadable name");
        return coopbridge::json::Value(std::string(name));
    });
    g_running = true;
    NFC_INFO("bridge: listening on 127.0.0.1:%u", port);
    return true;
}

void Bridge::pump() {
    if (g_running) g_server.Pump();
}

void Bridge::stop() {
    if (!g_running) return;
    g_server.Stop();
    g_running = false;
}

bool Bridge::enabled() { return g_running; }

}  // namespace nfc
