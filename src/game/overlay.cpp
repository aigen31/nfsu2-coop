#include "overlay.h"

#include <windows.h>
#include <d3d9.h>

#include <cstdio>

#include <MinHook.h>
#include "imgui.h"
#include "imgui_impl_dx9.h"

#include "core/log.h"

namespace nfc {
namespace {

using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);

EndSceneFn g_endScene = nullptr;
bool g_imguiReady = false;
bool g_hookTried = false;

void drawPanel(const HudState& s) {
    ImGui::SetNextWindowPos(ImVec2(18, 18), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.74f);
    ImGui::Begin("##nfsu2coop", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);

    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s  v%s", s.title.c_str(), versionString());
    ImGui::Separator();
    ImGui::TextUnformatted(s.status.c_str());
    if (!s.raceLine.empty()) ImGui::TextUnformatted(s.raceLine.c_str());
    if (!s.careerLine.empty()) ImGui::TextUnformatted(s.careerLine.c_str());

    if (!s.standings.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "standings");
        for (const std::string& line : s.standings) ImGui::TextUnformatted(line.c_str());
    }
    if (!s.remotes.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "co-op players");
        for (const std::string& line : s.remotes) ImGui::TextUnformatted(line.c_str());
    }
    if (!s.players.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "players");
        for (const std::string& line : s.players) ImGui::TextUnformatted(line.c_str());
    }
    if (!s.warnings.empty()) {
        ImGui::Separator();
        for (const std::string& line : s.warnings)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", line.c_str());
    }
    if (!s.notices.empty()) {
        ImGui::Separator();
        for (const std::string& line : s.notices)
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "%s", line.c_str());
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "F5 ready/start  F6 open event  F7 report event");
    ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "F8 hide  F9 resync  traffic %d  puppets %d",
                       s.traffic, s.puppets);
    ImGui::End();
}

HRESULT __stdcall hookEndScene(IDirect3DDevice9* device) {
    const HRESULT hr = g_endScene ? g_endScene(device) : D3D_OK;
    if (device) Overlay::instance().renderFrame(device);
    return hr;
}

IDirect3DDevice9* createDummyDevice() {
    static bool classReady = false;
    if (!classReady) {
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "NFC2OverlayDummy";
        RegisterClassExA(&wc);
        classReady = true;
    }
    HWND hwnd = CreateWindowExA(0, "NFC2OverlayDummy", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                GetModuleHandleA(nullptr), nullptr);
    if (!hwnd) return nullptr;
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        DestroyWindow(hwnd);
        return nullptr;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        pp.MultiSampleType = D3DMULTISAMPLE_NONE;
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp,
                               &dev);
    }
    d3d->Release();
    DestroyWindow(hwnd);
    if (FAILED(hr)) return nullptr;
    return dev;
}

}  // namespace

Overlay& Overlay::instance() {
    static Overlay o;
    return o;
}

bool Overlay::init() {
    if (installed_) return true;
    IDirect3DDevice9* dummy = createDummyDevice();
    if (!dummy) {
        NFC_WARN("overlay: cannot create a dummy D3D9 device, HUD disabled");
        return false;
    }
    void** vtbl = *reinterpret_cast<void***>(dummy);
    void* endScene = vtbl[42];  // IDirect3DDevice9::EndScene

    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) {
        NFC_WARN("overlay: MH_Initialize failed (%d)", static_cast<int>(ms));
        dummy->Release();
        return false;
    }
    if (MH_CreateHook(endScene, reinterpret_cast<LPVOID>(&hookEndScene), reinterpret_cast<LPVOID*>(&g_endScene)) !=
        MH_OK) {
        NFC_WARN("overlay: MH_CreateHook(EndScene) failed");
        dummy->Release();
        return false;
    }
    if (MH_EnableHook(endScene) != MH_OK) {
        NFC_WARN("overlay: MH_EnableHook(EndScene) failed");
        dummy->Release();
        return false;
    }
    dummy->Release();
    installed_ = true;
    g_hookTried = true;
    NFC_INFO("overlay: EndScene hook installed");
    return true;
}

void Overlay::shutdown() {
    if (!installed_) return;
    if (g_imguiReady) {
        ImGui_ImplDX9_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    installed_ = false;
}

void Overlay::setState(const HudState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    visible_ = state.visible;
}

void Overlay::toggle() {
    std::lock_guard<std::mutex> lock(mutex_);
    visible_ = !visible_;
}

bool Overlay::visible() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return visible_;
}

void Overlay::renderFrame(void* devicePtr) {
    IDirect3DDevice9* device = static_cast<IDirect3DDevice9*>(devicePtr);
    if (!device) return;
    if (!installed_) return;

    HudState copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!visible_) return;
        copy = state_;
    }
    if (gameThread_) gameThread_();

    if (!g_imguiReady) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();
        if (!ImGui_ImplDX9_Init(device)) {
            NFC_WARN("overlay: ImGui_ImplDX9_Init failed");
            ImGui::DestroyContext();
            return;
        }
        g_imguiReady = true;
        NFC_INFO("overlay: ImGui ready");
    }

    D3DVIEWPORT9 vp{};
    if (FAILED(device->GetViewport(&vp)) || vp.Width == 0) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(vp.Width), static_cast<float>(vp.Height));
    io.DeltaTime = 1.0f / 60.0f;

    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    drawPanel(copy);
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace nfc

