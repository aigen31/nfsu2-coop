// D3D9 HUD: ImGui drawn on EndScene. Hooks are installed by address of the
// device vtable, so they work with any D3D9 game and chain with other mods.
#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "hud.h"

namespace nfc {

class Overlay {
public:
    bool init();
    void shutdown();
    bool installed() const { return installed_; }
    void setState(const HudState& state);
    void toggle();
    bool visible() const;
    // Called by the EndScene hook; `device` is an IDirect3DDevice9*.
    void renderFrame(void* device);
    // Called on the render thread (the game's main thread) once per frame:
    // the adapter queues game-memory writes and runs them here.
    void setGameThreadHook(std::function<void()> fn) { gameThread_ = std::move(fn); }

    static Overlay& instance();

private:
    Overlay() = default;
    bool installed_ = false;
    mutable std::mutex mutex_;
    HudState state_;
    bool visible_ = true;
    std::function<void()> gameThread_;
};

}  // namespace nfc

