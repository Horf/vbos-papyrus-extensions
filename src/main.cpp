#include <exception>

#include <SKSE/API.h>
#include <SKSE/Interfaces.h>
#include <SKSE/Logger.h>

#include "PapyrusInterface.h"
#include "SubtitleManager.h"
#include "ConfigManager.h"
#include "RenderManager.h"
#include "RawAudioManager.h"

// Estimated offset (seconds) to bring the subtitles back in line with the audio after a savegame load while a book is playing
constexpr float LOAD_REWIND_SECONDS = 1.0f;

// Initialize messaging to load configs once data is ready
void InitializeMessaging() {
    auto* messaging = SKSE::GetMessagingInterface();
    messaging->RegisterListener([](SKSE::MessagingInterface::Message* message) {
        if (message->type == SKSE::MessagingInterface::kDataLoaded) {
			// Load subtitle settings from INI
            SubtitleManager::InstallInputSink();
            // Load INI mappings when Skyrim has loaded all forms
            try {
                ConfigManager::LoadConfigs();
            }
            catch (const std::exception& e) {
                logs::error("Loading the configuration files failed: {}", e.what());
            }
        }
        else if (message->type == SKSE::MessagingInterface::kPostPostLoad) {
            // Connect to VR ImGui helper if running in VR mode
            RenderManager::ConnectVR();
            // Connect to RAW Audio Engine if installed
            RawAudioManager::Initialize();
        }
        else if (message->type == SKSE::MessagingInterface::kPreLoadGame) {
            // A savegame is about to be loaded: end the running book here and keep its bookmark.
            // Otherwise RAW Audio Engine stopping its track would look like "book finished" and reset the bookmark.
            const bool wasRunning = RawAudioManager::Stop(false);
            SubtitleManager::StopSubtitles(-1, false);
            if (wasRunning) {
                // The audio resumes at the point where the load started, the subtitles are a bit ahead of it
                SubtitleManager::RewindBookBookmark(LOAD_REWIND_SECONDS);
            }
        }
    });
}

// SKSE Plugin Entry Point
SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    // Initialize SKSE and allocate 14 bytes for the trampoline in one step
    SKSE::Init(skse, { .trampoline = true, .trampolineSize = 14 });
    InitializeMessaging();
    logs::info("VBoS Extension Plugin loading...");

    RenderManager::InstallHooks();

    // Get the Papyrus interface to register our custom function.
    auto papyrus = SKSE::GetPapyrusInterface();
    if (!papyrus || !papyrus->Register(PapyrusInterface::Register)) {
        logs::error("Failed to register Papyrus function!");
        return false;
    }

    logs::info("VBoS Extension Plugin initialized successfully.\n");
    return true;
}