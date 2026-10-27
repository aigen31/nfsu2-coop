// Draw other players' cars by borrowing the game's own cars.
//
// The engine keeps a list of the cars it is about to draw at [0x89CCF8]
// (four Car* on the verified build; entry 0 is the local player's car). We hook
// CarRenderInfo::Render, and when it is about to draw one of the borrowed
// entries we overwrite that car's position/basis with a remote player's, let the
// game draw it, and restore the original values at EndScene. No second Render
// call, so the engine's render state is never re-entered.
#pragma once

#include <vector>

#include "core/common.h"

namespace nfc {

struct PuppetTarget {
    u32 model = 0;  // the owner's car model id, 0 = use the local player's
    u32 carPointer = 0;  // borrowed car object (the game's own)
    Vec3 pos;            // remote player's position
    float yaw = 0;       // radians, world Z is up
    float speed = 0;     // m/s, for the remote car's velocity/speed fields
};

class CarRenderer {
public:
    static CarRenderer& instance();

    bool install(u32 renderVa, bool modelSwap = false);
    u32 rideInfoOf(u32 car) const;
    void shutdown();
    bool ready() const { return installed_; }

    // Called from the session tick.
    void setTargets(u32 localCarPointer, const std::vector<PuppetTarget>& puppets);
    // Called from the EndScene hook (render thread): put the borrowed cars back
    // before the next simulation step.
    void restorePuppets();
    void clear();

private:
    CarRenderer() = default;
    bool installed_ = false;
};

}  // namespace nfc

