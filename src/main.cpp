#include <SKSE/API.h>
#include <SKSE/Interfaces.h>
#include <SKSE/Logger.h>
#include "PapyrusInterface.h"
#include "ConfigManager.h"

// Initialize messaging to load configs once data is ready
void InitializeMessaging() {
    auto* messaging = SKSE::GetMessagingInterface();
    messaging->RegisterListener([](SKSE::MessagingInterface::Message* message) {
        if (message->type == SKSE::MessagingInterface::kDataLoaded) {
            // Load INI mappings when Skyrim has loaded all forms
            ConfigManager::LoadConfigs();
        }
        });
}

// SKSE Plugin Entry Point
SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    // Initialize SKSE
    SKSE::Init(skse);
    InitializeMessaging();
    logs::info("VBoS Extension Plugin loading...");
	
    // Get the Papyrus interface to register our custom function.
    auto papyrus = SKSE::GetPapyrusInterface();
    if (!papyrus || !papyrus->Register(PapyrusInterface::Register)) {
        logs::error("Failed to register Papyrus function!");
        return false;
    }

    logs::info("VBoS Extension Plugin initialized successfully.");
    return true;
}