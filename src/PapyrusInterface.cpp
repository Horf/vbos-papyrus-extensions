#include "PapyrusInterface.h"
#include "ConfigManager.h"
#include "SubtitleManager.h"
#include "RenderManager.h"

#include <vector>
#include <cstdint>
#include <Windows.h>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <thread>
#include <string>

#include <RE/B/BGSSoundDescriptorForm.h>
#include <RE/B/BGSStandardSoundDef.h>
#include <RE/B/BSAudioManager.h>
#include <RE/B/BSCoreTypes.h>
#include <RE/B/BSFixedString.h>
#include <RE/B/BSSoundHandle.h>
#include <RE/F/FunctionArguments.h>
#include <RE/I/ID.h>
#include <RE/I/IFunctionArguments.h>
#include <RE/I/IVirtualMachine.h>
#include <RE/S/SkyrimVM.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectBOOK.h>
#include <RE/T/TypeTraits.h>

namespace PapyrusInterface
{
    // Active sound handles map (ID -> Handle)
    std::unordered_map<int32_t, RE::BSSoundHandle> playedSoundHandlesMap;
    
    // Mutexes for thread safety
    std::mutex soundMapMutex;
    std::mutex descriptorSwapMutex;

    RE::VMHandle GetHandle(RE::TESForm* akForm) {
        if (!akForm) return NULL;
        // Lazy singleton access: We get the VM instance here to avoid initialization order issues during plugin load.
        auto* vm = RE::SkyrimVM::GetSingleton();
        if (!vm) return NULL;
        // Cast the form type to the specific VMTypeID required by the handle policy.
        RE::VMTypeID id = static_cast<RE::VMTypeID>(akForm->GetFormType());
        return vm->GetVMRuntimeData().handlePolicy.GetHandleForObject(id, akForm);
    }

    void SendEvents(const std::vector<RE::VMHandle>& handles, RE::BSScript::IFunctionArguments* args) {
        auto* vm = RE::SkyrimVM::GetSingleton();
        // Safety check: If no handles or VM is missing, clean up arguments to prevent memory leaks.
        if (handles.empty() || !vm) {
            delete args;
            return;
        }
        // Static variable ensures initialization happens only once when needed (prevents crash on game load).
        static RE::BSFixedString sEventName = "OnSoundFinish";
        // Iterate through all handles (e.g., PlayerRef) and send the event.
        for (const auto& handle : handles) {
            vm->SendAndRelayEvent(handle, &sEventName, args, nullptr);
        }
        delete args;
    }

    void CreateSoundEvent(int32_t soundID, std::vector<RE::VMHandle> vmHandles, RE::TESForm* soundForm) {
        // Detached thread for non-blocking monitoring
        std::thread t([=]() {
            bool isPlaying = true;
            while (isPlaying) {
                // Poll every 100ms
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                // Lock the map before accessing it
                std::scoped_lock lock(soundMapMutex);
                auto it = playedSoundHandlesMap.find(soundID);
                
                if (it != playedSoundHandlesMap.end()) {
                    // Check if stopped (state 2) or invalid
                    if (it->second.state.underlying() == 2 || !it->second.IsValid()) {
                        isPlaying = false;
                        playedSoundHandlesMap.erase(it);
                    }
                }
                else {
                    // Handle was removed externally or not found.
                    isPlaying = false;
                }
            }

            // Stop subtitles when sound finishes automatically
            SubtitleManager::StopSubtitles(soundID);

            // Send finish event to Papyrus
            auto* args = RE::MakeFunctionArguments(static_cast<RE::TESForm*>(soundForm), static_cast<int>(soundID));
            SendEvents(vmHandles, args);
        });
        // Detach allows the thread to run independently; resources are released when it finishes.
        t.detach();
    }

    int32_t PlaySound(RE::StaticFunctionTag*, RE::TESObjectBOOK* akBook, RE::BGSSoundDescriptorForm* akTemplateDescriptor, float volume, RE::TESForm* eventReceiverForm) {
        if (!akBook || !akTemplateDescriptor) return -1;
        
		// Get path from INI config
        std::string soundFilePath = ConfigManager::GetPathForBook(akBook);
		// If no mapping exists, return -1
        if (soundFilePath.empty()) return -1;

        auto* audioManager = RE::BSAudioManager::GetSingleton();
        RE::BSSoundHandle soundHandle;
        bool built;
        
        {
            std::scoped_lock lock(descriptorSwapMutex);

            auto* internalDesc = akTemplateDescriptor->soundDescriptor;
            auto* standardDef = static_cast<RE::BGSStandardSoundDef*>(internalDesc);
            if (!standardDef || standardDef->soundFiles.empty()) return -1;

			// Swap ID to point to new file
            RE::BSResource::ID newFileID;
            newFileID.GenerateFromPath(soundFilePath.c_str());

            RE::BSResource::ID originalID = standardDef->soundFiles[0];
            standardDef->soundFiles[0] = newFileID;

            built = audioManager->GetSoundHandle(soundHandle, internalDesc);
            standardDef->soundFiles[0] = originalID;
        }
        if (!built || !soundHandle.IsValid()) return -1;
        
        // Set Volume and play.
		// NOTE: not setting a position on the sound handle 
        // forces the engine to treat it as a 2D/UI sound, 
        // which should prevent issues with menus/kill-cam/tfc/etc..
        soundHandle.SetVolume(volume);     
        if (soundHandle.Play()) {
            // Zombie Check: ID 0 means success but engine rejected file (bad path/format)
            if (soundHandle.soundID == 0) return -1;

            int32_t id = static_cast<int32_t>(soundHandle.soundID);

            // Register for event monitoring
            std::vector<RE::VMHandle> vmHandles;
            if (eventReceiverForm) {
                if (auto vh = GetHandle(eventReceiverForm)) vmHandles.push_back(vh);
            }
            {
                std::scoped_lock lock(soundMapMutex);
                playedSoundHandlesMap[id] = soundHandle;
            }

			// Start subtitles for the sound
            SubtitleManager::StartSubtitles(akBook, id);

            // Start monitoring the sound
            CreateSoundEvent(id, vmHandles, akBook);
            return id;
        }
        return -1;
    }

	// Papyrus function to pause/resume subtitles
    void SetSubtitlesPaused(RE::StaticFunctionTag*, bool bPaused) {
        SubtitleManager::SetPaused(bPaused);
    }

	// Papyrus function to enable/disable subtitles
    void EnableSubtitles(RE::StaticFunctionTag*, bool bEnable) {
        SubtitleManager::bSubtitlesEnabled = bEnable;
    }

	// Papyrus function to stop subtitles immediately
    void StopSubtitles(RE::StaticFunctionTag*) {
        SubtitleManager::StopSubtitles();
    }

	// Papyrus function to check if subtitles are available
    bool IsSubtitleAvailable(RE::StaticFunctionTag*) {
        return RenderManager::IsReady();
    }

    bool Register(RE::BSScript::IVirtualMachine* vm) {
        if (!vm) return false;
        vm->RegisterFunction("PlaySound", "VBoSPapyrusExtensions", PlaySound);
        vm->RegisterFunction("SetSubtitlesPaused", "VBoSPapyrusExtensions", SetSubtitlesPaused);
        vm->RegisterFunction("EnableSubtitles", "VBoSPapyrusExtensions", EnableSubtitles);
        vm->RegisterFunction("StopSubtitles", "VBoSPapyrusExtensions", StopSubtitles);
        vm->RegisterFunction("IsSubtitleAvailable", "VBoSPapyrusExtensions", IsSubtitleAvailable);
        return true;
    }
}