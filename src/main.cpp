#include <SKSE/API.h>
#include <SKSE/Interfaces.h>
#include <SKSE/Logger.h>
#include "papyrusFunctions.h"

// SKSE Plugin Entry Point
SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    // Initialize SKSE
    SKSE::Init(skse);
    logs::info("VBoS Papyrus Extensions loading...");
	
    // Get the Papyrus interface to register our custom functions.
    auto papyrus = SKSE::GetPapyrusInterface();
    if (!papyrus || !papyrus->Register(papyrusFunctions::Register)) {
        logs::error("Failed to register Papyrus functions!");
        return false;
    }

    logs::info("VBoS Papyrus Extensions initialized successfully.");
    return true;
}