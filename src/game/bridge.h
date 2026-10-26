// In-game bridge: the coop-anything RPC server (vendor/coopbridge/server.h)
// embedded in the plugin. Enable with [Debug] BridgePort in nfsu2coop.ini.
//
// Standard methods: ping, version, methods, events, state.
// NFSU2 methods: mem.read, mem.write, car.slots, car.name.
#pragma once

#include <cstdint>

namespace nfc {

class Bridge {
public:
    static bool start(std::uint16_t port);
    static void pump();  // once per frame, on the game thread
    static void stop();
    static bool enabled();
};

}  // namespace nfc
