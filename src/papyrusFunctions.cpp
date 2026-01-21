#include "papyrusFunctions.h"
#include <thread>
#include <map>
#include <vector>
#include <mutex>

namespace papyrusFunctions
{
    // Stores active sound handles to keep them alive and check their status.
    // Key: SoundID, Value: BSSoundHandle
    std::map<int, RE::BSSoundHandle> playedSoundHandlesMap;
    
    // Mutex to ensure thread-safe access to the map (preventing race conditions).
    std::mutex soundMapMutex;

    RE::VMHandle GetHandle(RE::TESForm* akForm) {
        if (!akForm) return NULL;

        // Lazy singleton access: We get the VM instance here to avoid initialization order issues during plugin load.
        auto* vm = RE::SkyrimVM::GetSingleton();
        if (!vm) return NULL;

        // Cast the form type to the specific VMTypeID required by the handle policy.
        RE::VMTypeID id = static_cast<RE::VMTypeID>(akForm->GetFormType());
        return vm->handlePolicy.GetHandleForObject(id, akForm);
    }

    void SendEvents(std::vector<RE::VMHandle> handles, RE::BSScript::IFunctionArguments* args) {
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

    void CreateSoundEvent(int soundID, std::vector<RE::VMHandle> vmHandles, RE::TESForm* soundForm) {
        // Launch a detached thread to monitor the sound without blocking the game.
        std::thread t([=]() {
            bool isPlaying = true;
            while (isPlaying) {
				// Poll every 100ms to check sound status. Should be efficient enough without causing performance issues.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                // Lock the map before accessing it.
                std::lock_guard<std::mutex> lock(soundMapMutex);
                auto it = playedSoundHandlesMap.find(soundID);
                
                if (it != playedSoundHandlesMap.end()) {
                    // State 2 typically means "Stopped" or "Finished".
                    // Also check if the handle has become invalid for other reasons.
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

            // Prepare arguments for the Papyrus event: (Form akSound, int instanceID)
            auto* args = RE::MakeFunctionArguments(static_cast<RE::TESForm*>(soundForm), static_cast<int>(soundID));
            SendEvents(vmHandles, args);
        });

        // Detach allows the thread to run independently; resources are released when it finishes.
        t.detach();
    }

    int PlaySound(RE::StaticFunctionTag*, RE::TESSound* akSound, float volume, RE::TESForm* eventReceiverForm) {
        // Validate inputs and dependencies.
        if (!akSound || !akSound->descriptor || !akSound->descriptor->soundDescriptor) return -1;
        
        auto* audioManager = RE::BSAudioManager::GetSingleton();
        if (!audioManager) return -1;
        
        RE::BSSoundHandle soundHandle;
        bool built = audioManager->BuildSoundDataFromDescriptor(soundHandle, akSound->descriptor->soundDescriptor);        
        
        if (!built || !soundHandle.IsValid()) return -1;
        
        // Set Volume and play.
		// NOTE: not setting a position on the sound handle 
        // forces the engine to treat it as a 2D/UI sound, 
        // which should prevent issues with menus/kill-cam/tfc/etc..
        soundHandle.SetVolume(volume);
        
        if (!soundHandle.Play()) return -1;
        
        int id = soundHandle.soundID;
        
        // Prepare the handle for the script that wants to receive the "OnSoundFinish" event.
        std::vector<RE::VMHandle> vmHandles;
        if (eventReceiverForm) {
            RE::VMHandle vmHandle = GetHandle(eventReceiverForm);
            if (vmHandle != NULL) {
                vmHandles.push_back(vmHandle);
            }
        }

        // Register the sound handle in our map (thread-safe) so the waiter thread can find it.
        {
            std::lock_guard<std::mutex> lock(soundMapMutex);
            playedSoundHandlesMap[id] = soundHandle;
        }
        
        // Start monitoring the sound.
        CreateSoundEvent(id, vmHandles, akSound);
        return id;
    }

    RE::BGSSoundCategory* GetSoundCategoryForSoundDescriptor(RE::StaticFunctionTag*, RE::BGSSoundDescriptorForm* akSoundDescriptor) {
        if (!akSoundDescriptor || !akSoundDescriptor->soundDescriptor) return nullptr;
        return akSoundDescriptor->soundDescriptor->category;
    }

    bool Register(RE::BSScript::IVirtualMachine* vm) {
        if (!vm) return false;
        std::string className = "VBoSPapyrusExtensions";

        // Register functions to be called from Papyrus.
        vm->RegisterFunction("PlaySound", className, PlaySound);
        vm->RegisterFunction("GetSoundCategoryForSoundDescriptor", className, GetSoundCategoryForSoundDescriptor);
        return true;
    }
}