#include "RenderManager.h"
#include "SubtitleManager.h"
#include "ImGuiVRHelperClientSDK.h"
#include "ImGuiVRHelperTypes.h"

#include <Windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <atomic>
#include <memory>
#include <cstdarg>

#include <RE/Offsets_VTABLE.h>
#include <RE/I/IMenu.h>
#include <RE/R/Renderer.h>

#include <REL/Module.h>
#include <REL/Relocation.h>

#include <SKSE/Impl/PCH.h>
#include <SKSE/Logger.h>
#include <SKSE/Trampoline.h>

using namespace std::literals;

namespace RenderManager
{
    std::atomic<bool> initialized{ false };

    ImGuiVRHelperPluginAPI::Client g_vrClient;
    ID3D11Device* g_d3dDevice = nullptr;
    ID3D11DeviceContext* g_d3dContext = nullptr;

    void ConnectVR() {
        if (REL::Module::IsVR()) {
            if (g_vrClient.Connect("VBoS", "2.2.1", ImGuiVRHelperPluginAPI::kClientFlag_HUDMode)) {
                logs::info("Connected to ImGuiVRHelper (HUD Mode)!");

                g_vrClient.SetHudStyleCallback([]() {
                    SubtitleManager::LoadSettings();
                    auto& io = ImGui::GetIO();
                    SubtitleManager::LoadFont(io);
                    });
            }
            else {
                logs::warn("ImGuiVRHelper not found or connection failed. VR subtitles won't be available.");
            }
        }
    }

    // Hook to initialize ImGui when the DirectX swap chain is created
    struct CreateD3DAndSwapChain
    {
        static void thunk()
        {
            func();

            if (const auto renderer = RE::BSGraphics::Renderer::GetSingleton()) {

                const auto swapChain = reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain);
                if (!swapChain) {
                    logs::error("couldn't find swapChain");
                    return;
                }

                DXGI_SWAP_CHAIN_DESC desc{};
                if (FAILED(swapChain->GetDesc(std::addressof(desc)))) return;

                const auto device = reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder);
                const auto context = reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context);

                if (REL::Module::IsVR()) {
                    logs::info("Initializing ImGui for VR...");
                    g_d3dDevice = device;
                    g_d3dContext = context;
                }
                else {
                    logs::info("Initializing ImGui...");

                    ImGui::CreateContext();
                    auto& io = ImGui::GetIO();
                    io.IniFilename = nullptr;

                    SubtitleManager::LoadSettings();
                    SubtitleManager::LoadFont(io);

                    if (!ImGui_ImplWin32_Init(desc.OutputWindow)) return;
                    if (!ImGui_ImplDX11_Init(device, context)) return;
                }
                logs::info("ImGui initialized.");
                initialized.store(true);
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // Hook into the HUD menu to render ImGui elements (subtitles) over the game world
    struct PostDisplay
    {
        static void thunk(RE::IMenu* a_menu)
        {
            func(a_menu);

            if (!initialized.load()) return;

            if (REL::Module::IsVR()) {
                if (!g_vrClient.IsConnected()) return;

                static const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
                const ImVec2 displaySize{ static_cast<float>(screenSize.width), static_cast<float>(screenSize.height) };

                g_vrClient.RenderHud(g_d3dDevice, g_d3dContext, displaySize, []() {
                    GImGui->NavWindowingTarget = nullptr;
                    SubtitleManager::Render();
                    });
            }
            else {
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();

                {
                    static const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();

                    auto& io = ImGui::GetIO();
                    io.DisplaySize.x = static_cast<float>(screenSize.width);
                    io.DisplaySize.y = static_cast<float>(screenSize.height);
                }

                ImGui::NewFrame();

                {
                    GImGui->NavWindowingTarget = nullptr;
                    SubtitleManager::Render();
                }

                ImGui::EndFrame();
                ImGui::Render();
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static inline std::size_t idx{ 0x6 };
    };

    void InstallHooks()
    {
        auto& trampoline = SKSE::GetTrampoline();

        REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(75595, 77226), REL::Relocate(0x9, 0x275) };
        CreateD3DAndSwapChain::func = trampoline.write_call<5>(target.address(), CreateD3DAndSwapChain::thunk);

        REL::Relocation<std::uintptr_t> vtable(RE::VTABLE_HUDMenu[0]);
        PostDisplay::func = vtable.write_vfunc(PostDisplay::idx, PostDisplay::thunk);

        logs::info("Engine Hooks for ImGui installed successfully.");
    }

    bool IsReady() {
        if (REL::Module::IsVR()) {
            return initialized.load() && g_vrClient.IsConnected();
        }
        return initialized.load();
    }
}