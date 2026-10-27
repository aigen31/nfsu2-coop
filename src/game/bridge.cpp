#include "bridge.h"

#include "render_cars.h"

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
    // Rebuild a car's model/render object with another model. FUN_0063a5a0(car, model)
    // mallocs 0x2EC0 bytes, builds the model via FUN_00638190(model, car) and refreshes
    // the draw batches (see build/decomp/all.c). Runs on the main thread via Pump().
    g_server.Register("car.reload", [](const coopbridge::json::Value& p) {
        const std::uint32_t slot = static_cast<std::uint32_t>(p.get("slot").asInt(1));
        std::uint32_t car = 0;
        if (!readMem(0x89CCF8 + slot * 4, &car, sizeof(car)) || !car)
            throw coopbridge::Error(-32013, "no car in that slot");
        std::uint32_t model = 0;
        std::uint32_t local = 0;
        std::uint32_t desc = 0;
        if (readMem(0x89CCF8, &local, sizeof(local)) && local && readMem(local + 0x554, &desc, sizeof(desc)) && desc)
            readMem(desc + 0x04, &model, sizeof(model));
        const auto override = p.get("model").asInt(0);
        if (override) model = static_cast<std::uint32_t>(override);
        if (!model) throw coopbridge::Error(-32015, "no model to apply");
        using ReloadFn = void(__thiscall*)(void*, std::uint32_t);
        auto fn = reinterpret_cast<ReloadFn>(static_cast<std::uintptr_t>(0x0063a5a0));
        fn(reinterpret_cast<void*>(static_cast<std::uintptr_t>(car)), model);
        auto r = coopbridge::json::Value::object();
        r.set("car", static_cast<std::int64_t>(car));
        r.set("model", static_cast<std::int64_t>(model));
        return r;
    });

    // Rebuild a car's render object from a model *name* (the engine loads the model by
    // the name at car+0x0C). The name lives in our own buffer, so it works for names the
    // game has not loaded yet.
    g_server.Register("car.rebuildName", [](const coopbridge::json::Value& p) {
        static char nameBuf[64];
        const std::uint32_t slot = static_cast<std::uint32_t>(p.get("slot").asInt(1));
        std::uint32_t car = 0;
        if (!readMem(0x89CCF8 + slot * 4, &car, sizeof(car)) || !car)
            throw coopbridge::Error(-32013, "no car in that slot");
        const std::string name = p.get("name").asString();
        if (name.empty() || name.size() >= sizeof(nameBuf)) throw coopbridge::Error(-32016, "bad name");
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", name.c_str());
        std::uint32_t model = static_cast<std::uint32_t>(p.get("model").asInt(0));
        if (!model) {
            std::uint32_t local = 0, desc = 0;
            if (readMem(0x89CCF8, &local, sizeof(local)) && local &&
                readMem(local + 0x554, &desc, sizeof(desc)) && desc)
                readMem(desc + 0x04, &model, sizeof(model));
        }
        std::uint32_t oldName = 0;
        if (!readMem(car + 0x0C, &oldName, sizeof(oldName))) throw coopbridge::Error(-32011, "unreadable car");
        const std::uint32_t mine = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(nameBuf));
        writeMem(car + 0x0C, &mine, sizeof(mine));
        using ReloadFn = void(__thiscall*)(void*, std::uint32_t);
        reinterpret_cast<ReloadFn>(static_cast<std::uintptr_t>(0x0063A5A0))(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(car)), model);
        writeMem(car + 0x0C, &oldName, sizeof(oldName));
        auto r = coopbridge::json::Value::object();
        r.set("car", static_cast<std::int64_t>(car));
        r.set("name", name);
        r.set("model", static_cast<std::int64_t>(model));
        std::uint32_t desc = 0, namePtr = 0;
        if (readMem(car + 0x554, &desc, sizeof(desc)) && desc && readMem(desc + 0x08, &namePtr, sizeof(namePtr)) && namePtr) {
            char descName[32] = {};
            if (readMem(namePtr, descName, sizeof(descName) - 1)) r.set("descName", std::string(descName));
        }
        return r;
    });

    // Rebuild the car's visual root (the object at car+0x00, the one FUN_0063A5A0
    // built at spawn) with another model id.
    g_server.Register("car.rebuildRoot", [](const coopbridge::json::Value& p) {
        const std::uint32_t slot = static_cast<std::uint32_t>(p.get("slot").asInt(1));
        std::uint32_t car = 0;
        if (!readMem(0x89CCF8 + slot * 4, &car, sizeof(car)) || !car)
            throw coopbridge::Error(-32013, "no car in that slot");
        std::uint32_t root = 0;
        if (!readMem(car, &root, sizeof(root)) || !root) throw coopbridge::Error(-32011, "no root object");
        std::uint32_t model = static_cast<std::uint32_t>(p.get("model").asInt(0));
        if (!model) throw coopbridge::Error(-32016, "model required");
        using ReloadFn = void(__thiscall*)(void*, std::uint32_t);
        reinterpret_cast<ReloadFn>(static_cast<std::uintptr_t>(0x0063A5A0))(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(root)), model);
        auto r = coopbridge::json::Value::object();
        r.set("car", static_cast<std::int64_t>(car));
        r.set("root", static_cast<std::int64_t>(root));
        r.set("model", static_cast<std::int64_t>(model));
        std::uint32_t desc = 0, namePtr = 0;
        if (readMem(root + 0x554, &desc, sizeof(desc)) && desc && readMem(desc + 0x08, &namePtr, sizeof(namePtr)) && namePtr) {
            char descName[32] = {};
            if (readMem(namePtr, descName, sizeof(descName) - 1)) r.set("descName", std::string(descName));
        }
        return r;
    });

    g_server.Register("car.setRoot", [](const coopbridge::json::Value& p) {
        const std::uint32_t slot = static_cast<std::uint32_t>(p.get("slot").asInt(1));
        std::uint32_t car = 0;
        if (!readMem(0x89CCF8 + slot * 4, &car, sizeof(car)) || !car)
            throw coopbridge::Error(-32013, "no car in that slot");
        std::uint32_t root = static_cast<std::uint32_t>(p.get("root").asInt(0));
        if (!root) throw coopbridge::Error(-32016, "root required");
        std::uint32_t old = 0;
        readMem(car, &old, sizeof(old));
        writeMem(car, &root, sizeof(root));
        auto r = coopbridge::json::Value::object();
        r.set("car", static_cast<std::int64_t>(car));
        r.set("oldRoot", static_cast<std::int64_t>(old));
        r.set("root", static_cast<std::int64_t>(root));
        return r;
    });

    // The world keeps cars as nodes in DAT_00890080 (counts at +0x10/+0x14/+0x18, array at
    // +0x1c). Each node points at its car data (+0x10) and car object (+0x14). This dumps
    // them so we can find the node behind a render-list car.
    g_server.Register("world.cars", [](const coopbridge::json::Value&) {
        auto out = coopbridge::json::Value::array();
        std::uint32_t world = 0;
        if (!readMem(0x00890080, &world, sizeof(world)) || !world)
            throw coopbridge::Error(-32017, "world not ready");
        std::int32_t lo = 0, hi = 0, count = 0;
        readMem(world + 0x10, &count, sizeof(count));
        readMem(world + 0x14, &lo, sizeof(lo));
        readMem(world + 0x18, &hi, sizeof(hi));
        if (lo < 0 || hi < lo || hi - lo > 64) return out;
        for (std::int32_t i = lo; i < hi; ++i) {
            std::uint32_t node = 0, data = 0, car = 0, model = 0, idx = 0;
            if (!readMem(world + 0x1c + static_cast<std::uint32_t>(i - lo) * 4, &node, sizeof(node)) || !node) continue;
            readMem(node + 0x10, &data, sizeof(data));
            readMem(node + 0x14, &car, sizeof(car));
            if (data) readMem(data, &model, sizeof(model));
            readMem(node + 0x08, &idx, sizeof(idx));
            auto e = coopbridge::json::Value::object();
            e.set("i", static_cast<std::int64_t>(i));
            e.set("node", static_cast<std::int64_t>(node));
            e.set("data", static_cast<std::int64_t>(data));
            e.set("car", static_cast<std::int64_t>(car));
            e.set("model", static_cast<std::int64_t>(model));
            e.set("index", static_cast<std::int64_t>(idx));
            out.push(e);
        }
        return out;
    });

    // Set the car's model table (car+0x10) and rebuild its render object the same way the
    // game does at spawn: FUN_0063A5A0(car, *(car+0x10)).
    g_server.Register("car.setModel", [](const coopbridge::json::Value& p) {
        const std::uint32_t slot = static_cast<std::uint32_t>(p.get("slot").asInt(1));
        std::uint32_t car = 0;
        if (!readMem(0x89CCF8 + slot * 4, &car, sizeof(car)) || !car)
            throw coopbridge::Error(-32013, "no car in that slot");
        std::uint32_t model = static_cast<std::uint32_t>(p.get("model").asInt(0));
        if (!model) throw coopbridge::Error(-32016, "model required");
        std::uint32_t oldModel = 0;
        readMem(car + 0x10, &oldModel, sizeof(oldModel));
        writeMem(car + 0x10, &model, sizeof(model));
        using ReloadFn = void(__thiscall*)(void*, std::uint32_t);
        reinterpret_cast<ReloadFn>(static_cast<std::uintptr_t>(0x0063A5A0))(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(car)), model);
        auto r = coopbridge::json::Value::object();
        r.set("car", static_cast<std::int64_t>(car));
        r.set("oldModel", static_cast<std::int64_t>(oldModel));
        r.set("model", static_cast<std::int64_t>(model));
        std::uint32_t d = 0, np = 0;
        if (readMem(car + 0x554, &d, sizeof(d)) && d && readMem(d + 0x08, &np, sizeof(np)) && np) {
            char nm[32] = {};
            if (readMem(np, nm, sizeof(nm) - 1)) r.set("descName", std::string(nm));
        }
        return r;
    });

    // GetCarTypeInfo(CarTypeID) @ 0x610110 gives the game's own car-type record (name at +0).
    g_server.Register("game.carType", [](const coopbridge::json::Value& p) {
        const int id = static_cast<int>(p.get("id").asInt(0));
        using Fn = void* (*)(int);
        auto fn = reinterpret_cast<Fn>(static_cast<std::uintptr_t>(0x610110));
        void* info = fn(id);
        auto r = coopbridge::json::Value::object();
        r.set("id", static_cast<std::int64_t>(id));
        r.set("ptr", static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(info)));
        if (info) {
            char name[40] = {};
            if (readMem(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(info)), name, sizeof(name) - 1))
                r.set("name", std::string(name));
            std::uint32_t hash = 0;
            if (readMem(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(info)) + 0xC0, &hash, sizeof(hash)))
                r.set("hash", static_cast<std::int64_t>(hash));
        }
        return r;
    });

    // car+0x0C is the CarTypeInfo* (its first field is the type name). Swapping it and
    // rebuilding the render object is the game's own way to change a car's model.
    g_server.Register("car.setType", [](const coopbridge::json::Value& p) {
        const std::uint32_t slot = static_cast<std::uint32_t>(p.get("slot").asInt(1));
        const int typeId = static_cast<int>(p.get("type").asInt(0));
        std::uint32_t car = 0;
        if (!readMem(0x89CCF8 + slot * 4, &car, sizeof(car)) || !car)
            throw coopbridge::Error(-32013, "no car in that slot");
        using TypeFn = void* (*)(int);
        void* info = reinterpret_cast<TypeFn>(static_cast<std::uintptr_t>(0x610110))(typeId);
        if (!info) throw coopbridge::Error(-32018, "no such car type");
        const std::uint32_t infoPtr = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(info));
        std::uint32_t oldType = 0;
        readMem(car + 0x0C, &oldType, sizeof(oldType));
        writeMem(car + 0x0C, &infoPtr, sizeof(infoPtr));
        // Player cars share the base render table (0x890170/0x890160); the model itself
        // comes from CarTypeInfo at car+0x0C.
        std::uint32_t model = 0x00890170u, model2 = 0x00890160u;
        writeMem(car + 0x10, &model, sizeof(model));
        writeMem(car + 0x14, &model2, sizeof(model2));
        using ReloadFn = void(__thiscall*)(void*, std::uint32_t);
        reinterpret_cast<ReloadFn>(static_cast<std::uintptr_t>(0x0063A5A0))(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(car)), model);
        auto r = coopbridge::json::Value::object();
        r.set("car", static_cast<std::int64_t>(car));
        r.set("oldType", static_cast<std::int64_t>(oldType));
        r.set("type", static_cast<std::int64_t>(infoPtr));
        std::uint32_t d = 0, np = 0;
        if (readMem(car + 0x554, &d, sizeof(d)) && d && readMem(d + 0x08, &np, sizeof(np)) && np) {
            char nm[32] = {};
            if (readMem(np, nm, sizeof(nm) - 1)) r.set("descName", std::string(nm));
        }
        return r;
    });

    // Change the car's CarTypeID in its RideInfo (RideInfo[0]) and rebuild the parts with
    // CarRenderInfo_UpdateCarParts @ 0x634800.
    g_server.Register("car.setRideType", [](const coopbridge::json::Value& p) {
        const std::uint32_t slot = static_cast<std::uint32_t>(p.get("slot").asInt(1));
        const int typeId = static_cast<int>(p.get("type").asInt(0));
        std::uint32_t car = 0;
        if (!readMem(0x89CCF8 + slot * 4, &car, sizeof(car)) || !car)
            throw coopbridge::Error(-32013, "no car in that slot");
        const std::uint32_t ride = CarRenderer::instance().rideInfoOf(car);
        if (!ride) throw coopbridge::Error(-32019, "ride info unknown for that car");
        std::uint32_t oldType = 0;
        readMem(ride, &oldType, sizeof(oldType));
        const std::uint32_t want = static_cast<std::uint32_t>(typeId);
        writeMem(ride, &want, sizeof(want));
        std::uint32_t renderInfo = 0;
        readMem(car + 0x554, &renderInfo, sizeof(renderInfo));
        if (renderInfo) {
            using UpdFn = void(__thiscall*)(void*);
            reinterpret_cast<UpdFn>(static_cast<std::uintptr_t>(0x634800))(
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(renderInfo)));
        }
        auto r = coopbridge::json::Value::object();
        r.set("car", static_cast<std::int64_t>(car));
        r.set("ride", static_cast<std::int64_t>(ride));
        r.set("oldType", static_cast<std::int64_t>(oldType));
        r.set("type", static_cast<std::int64_t>(want));
        std::uint32_t d = 0, np = 0;
        if (readMem(car + 0x554, &d, sizeof(d)) && d && readMem(d + 0x08, &np, sizeof(np)) && np) {
            char nm[32] = {};
            if (readMem(np, nm, sizeof(nm) - 1)) r.set("descName", std::string(nm));
        }
        return r;
    });

    // The per-type RideInfo objects live in .data at 0x890170 + n*0x940; their first dword
    // is the CarTypeID. Build a car's render object again from the RideInfo of the wanted
    // type (CarRenderInfo_ctor @ 0x638190) and refresh its parts (0x634800).
    g_server.Register("car.applyType", [](const coopbridge::json::Value& p) {
        const std::uint32_t slot = static_cast<std::uint32_t>(p.get("slot").asInt(1));
        const std::uint32_t typeId = static_cast<std::uint32_t>(p.get("type").asInt(0));
        std::uint32_t car = 0;
        if (!readMem(0x89CCF8 + slot * 4, &car, sizeof(car)) || !car)
            throw coopbridge::Error(-32013, "no car in that slot");
        std::uint32_t ride = 0;
        for (std::uint32_t n = 0; n < 64 && !ride; ++n) {
            std::uint32_t entry = 0x00890170u + n * 0x940u;
            std::uint32_t id = 0, info = 0;
            if (!readMem(entry, &id, sizeof(id))) break;
            if (!readMem(entry + 0x04, &info, sizeof(info))) continue;  // must look like a RideInfo
            if (id == typeId && info) ride = entry;
        }
        if (!ride) throw coopbridge::Error(-32020, "no ride info for that type");
        std::uint32_t renderInfo = 0;
        if (!readMem(car + 0x554, &renderInfo, sizeof(renderInfo)) || !renderInfo)
            throw coopbridge::Error(-32017, "no render info");
        using CtorFn = void*(__thiscall*)(void*, void*, void*);
        reinterpret_cast<CtorFn>(static_cast<std::uintptr_t>(0x638190))(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(renderInfo)),
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(ride)),
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(car)));
        using UpdFn = void(__thiscall*)(void*);
        reinterpret_cast<UpdFn>(static_cast<std::uintptr_t>(0x634800))(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(renderInfo)));
        auto r = coopbridge::json::Value::object();
        r.set("car", static_cast<std::int64_t>(car));
        r.set("ride", static_cast<std::int64_t>(ride));
        r.set("type", static_cast<std::int64_t>(typeId));
        std::uint32_t d = 0, np = 0;
        if (readMem(car + 0x554, &d, sizeof(d)) && d && readMem(d + 0x08, &np, sizeof(np)) && np) {
            char nm[32] = {};
            if (readMem(np, nm, sizeof(nm) - 1)) r.set("descName", std::string(nm));
        }
        return r;
    });

    // Build a RideInfo for a type that is not loaded yet (RideInfo_Init @ 0x610270) and give
    // it to the car's render object (CarRenderInfo_ctor @ 0x638190 + UpdateCarParts @ 0x634800).
    // This is the path that makes the engine stream the model of that type.
    g_server.Register("car.applyNewType", [](const coopbridge::json::Value& p) {
        static std::uint8_t ride[0x2000];
        const std::uint32_t slot = static_cast<std::uint32_t>(p.get("slot").asInt(1));
        const int typeId = static_cast<int>(p.get("type").asInt(0));
        std::uint32_t car = 0;
        if (!readMem(0x89CCF8 + slot * 4, &car, sizeof(car)) || !car)
            throw coopbridge::Error(-32013, "no car in that slot");
        std::memset(ride, 0, sizeof(ride));
        using InitFn = void(__thiscall*)(void*, int, int, int, int);
        reinterpret_cast<InitFn>(static_cast<std::uintptr_t>(0x610270))(
            ride, typeId, 0, 1, 1);
        std::uint32_t renderInfo = 0;
        if (!readMem(car + 0x554, &renderInfo, sizeof(renderInfo)) || !renderInfo)
            throw coopbridge::Error(-32017, "no render info");
        using CtorFn = void*(__thiscall*)(void*, void*, void*);
        reinterpret_cast<CtorFn>(static_cast<std::uintptr_t>(0x638190))(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(renderInfo)),
            static_cast<void*>(ride),
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(car)));
        using UpdFn = void(__thiscall*)(void*);
        reinterpret_cast<UpdFn>(static_cast<std::uintptr_t>(0x634800))(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(renderInfo)));
        auto r = coopbridge::json::Value::object();
        r.set("car", static_cast<std::int64_t>(car));
        r.set("type", static_cast<std::int64_t>(typeId));
        r.set("rideType", static_cast<std::int64_t>(*reinterpret_cast<std::uint32_t*>(ride)));
        std::uint32_t d = 0, np = 0;
        if (readMem(car + 0x554, &d, sizeof(d)) && d && readMem(d + 0x08, &np, sizeof(np)) && np) {
            char nm[32] = {};
            if (readMem(np, nm, sizeof(nm) - 1)) r.set("descName", std::string(nm));
        }
        return r;
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
