#pragma once

#include <atomic>

namespace RenderManager
{
    // Installs engine hooks required for ImGui integration
    void InstallHooks();

    // Connects to the VR HUD plugin if the game is running in VR mode
    void ConnectVR();

    // Returns true if ImGui and/or VR rendering are fully initialized
    bool IsReady();

    extern std::atomic<bool> initialized;
}