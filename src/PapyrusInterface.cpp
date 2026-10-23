#include "PapyrusInterface.h"
#include "ConfigManager.h"
#include "SubtitleManager.h"
#include "RenderManager.h"
#include "RawAudioManager.h"

#include <algorithm>
#include <atomic>
#include <vector>
#include <cstdint>
#include <Windows.h>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <thread>
#include <string>
#include <exception>

#include <SKSE/Logger.h>

#include <RE/B/BGSSoundDescriptorForm.h>
#include <RE/B/BGSStandardSoundDef.h>
#include <RE/B/BSAudioManager.h>
#include <RE/B/BSCoreTypes.h>
#include <RE/B/BSFixedString.h>
#include <RE/B/BSSoundHandle.h>
#include <RE/F/FunctionArguments.h>
#include <RE/I/ID.h>
#include <RE/I/IFunctionArguments.h>
#include <RE/I/INIPrefSettingCollection.h>
#include <RE/I/IVirtualMachine.h>
#include <RE/S/Setting.h>
#include <RE/S/SkyrimVM.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectBOOK.h>
#include <RE/T/TypeTraits.h>

namespace PapyrusInterface
{
    // Active sound handles map (ID -> Handle)
    std::unordered_map<int32_t, RE::BSSoundHandle> playedSoundHandlesMap;

    // Synthetic ID counter for RAW Audio sessions
    static std::atomic<int32_t> rawSessionCounter{ 1000000 };
    static RE::FormID lastPlayedBookID = 0;

    // Mutexes for thread safety
    std::mutex soundMapMutex;
    std::mutex descriptorSwapMutex;

    // Reads Master Volume (from INI, ignoring background ducking) * VBoS Category Volume
    static float CalculateBookGain(RE::BGSSoundDescriptorForm* a_descriptor, float a_baseVolume) {
        // Setting objects live as long as the game, so the lookup is only done once
        static std::atomic<RE::Setting*> masterSetting{ nullptr };
        auto* setting = masterSetting.load();
        if (!setting) {
            if (auto* iniPrefs = RE::INIPrefSettingCollection::GetSingleton()) {
                setting = iniPrefs->GetSetting("fAudioMasterVolume:AudioMenu");
                masterSetting.store(setting);
            }
        }
        const float masterVol = setting ? setting->GetFloat() : 1.0f;

        float categoryVol = 1.0f;
        if (a_descriptor && a_descriptor->soundDescriptor) {
            auto* standardDef = static_cast<RE::BGSStandardSoundDef*>(a_descriptor->soundDescriptor);
            if (standardDef && standardDef->category) {
                categoryVol = standardDef->category->GetCategoryVolume();
            }
        }

        return std::clamp(masterVol * categoryVol * a_baseVolume, 0.0f, 2.0f);
    }

    RE::VMHandle GetHandle(RE::TESForm* akForm) {
        if (!akForm) return NULL;
        // Get the VM instance here to avoid initialization order issues during plugin load
        auto* vm = RE::SkyrimVM::GetSingleton();
        if (!vm) return NULL;
        // Cast the form type to the specific VMTypeID required by the handle policy
        RE::VMTypeID id = static_cast<RE::VMTypeID>(akForm->GetFormType());
        return vm->GetVMRuntimeData().handlePolicy.GetHandleForObject(id, akForm);
    }

    void SendEvents(const std::vector<RE::VMHandle>& handles, RE::BSScript::IFunctionArguments* args) {
        auto* vm = RE::SkyrimVM::GetSingleton();
        // If no handles or VM is missing, clean up arguments to prevent memory leaks
        if (handles.empty() || !vm) {
            delete args;
            return;
        }
        // Static variable ensures initialization happens only once when needed (prevents crash on game load)
        static RE::BSFixedString sEventName = "OnSoundFinish";
        // Iterate through all handles (e.g., PlayerRef) and send the event
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
                    // Handle was removed externally or not found
                    isPlaying = false;
                }
            }

            // Stop subtitles when sound finishes automatically
            SubtitleManager::StopSubtitles(soundID);

            // Send finish event to Papyrus
            auto* args = RE::MakeFunctionArguments(static_cast<RE::TESForm*>(soundForm), static_cast<int>(soundID));
            SendEvents(vmHandles, args);
            });
        // Detach allows the thread to run independently; resources are released when it finishes
        t.detach();
    }

    // Monitoring thread for RAW Audio Engine playback
    static void CreateRawSoundEvent(int32_t sessionID, std::vector<RE::VMHandle> vmHandles, RE::TESObjectBOOK* bookForm, RE::BGSSoundDescriptorForm* descriptorForm, float baseVolume) {
        std::thread t([=]() {
            try {
                bool finishedNaturally = false;

                while (true) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    // If session was stopped externally (book closed, stop key pressed, or new book opened), exit
                    if (!RawAudioManager::IsSessionActive(sessionID)) {
                        break;
                    }

                    // If session is still active, but RAW engine finished streaming the file -> Natural finish
                    if (!RawAudioManager::IsPlaying()) {
                        finishedNaturally = true;
                        break;
                    }

                    // Keep volume synced with Skyrim's Audio Menu slider
                    RawAudioManager::SetVolume(CalculateBookGain(descriptorForm, baseVolume));
                }

                // Book was read to the very end: end the session (audio bookmark reset to 0:00)
                // FinishSession only acts if the session is still ours, a newly started book stays untouched
                if (finishedNaturally && RawAudioManager::FinishSession(sessionID, true)) {
                    SubtitleManager::StopSubtitles(sessionID, true);

                    auto* args = RE::MakeFunctionArguments(static_cast<RE::TESForm*>(bookForm), static_cast<int>(sessionID));
                    SendEvents(vmHandles, args);
                }
            }
            catch (const std::exception& e) {
                logs::error("RAW Audio: Monitoring thread stopped by an exception: {}", e.what());
            }
        });
        t.detach();
    }

    int32_t PlaySound(RE::StaticFunctionTag*, RE::TESObjectBOOK* akBook, RE::BGSSoundDescriptorForm* akTemplateDescriptor, float volume, RE::TESForm* eventReceiverForm) {
        if (!akBook || !akTemplateDescriptor) return -1;

        // Get path from INI config
        std::string soundFilePath = ConfigManager::GetPathForBook(akBook);
        // If no mapping exists, return -1
        if (soundFilePath.empty()) return -1;

        lastPlayedBookID = akBook->GetFormID();

        // Try RAW Audio Engine (if installed and file is resolvable)
        try {
            if (RawAudioManager::IsAvailable()) {
                std::string resolvedPath = RawAudioManager::ResolveAudioPath(soundFilePath);
                if (!resolvedPath.empty()) {
                    int32_t sessionID = ++rawSessionCounter;
                    float gain = CalculateBookGain(akTemplateDescriptor, volume);

                    if (RawAudioManager::Play(akBook->GetFormID(), resolvedPath, gain, sessionID)) {
                        std::vector<RE::VMHandle> vmHandles;
                        if (eventReceiverForm) {
                            if (auto vh = GetHandle(eventReceiverForm)) vmHandles.push_back(vh);
                        }

                        SubtitleManager::StartSubtitles(akBook, sessionID, true);
                        CreateRawSoundEvent(sessionID, vmHandles, akBook, akTemplateDescriptor, volume);
                        return sessionID;
                    }
                }
            }
        }
        catch (const std::exception& e) {
            logs::error("RAW Audio: Setup failed ({}). Falling back to the Skyrim audio engine.", e.what());
            RawAudioManager::Stop(false);
        }

        // Default Skyrim Audio Engine
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

		// Set Volume and play (no position set to force 2D/UI sound)
        soundHandle.SetVolume(volume);
        if (soundHandle.Play()) {
            // ID 0 means success but engine rejected file (bad path/format)
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

    // Papyrus function to pause/resume subtitles (and RAW audio if active)
    void SetSubtitlesPaused(RE::StaticFunctionTag*, bool bPaused) {
        RawAudioManager::SetPaused(bPaused);
        SubtitleManager::SetPaused(bPaused);
    }

    // Papyrus function to enable/disable subtitles
    void EnableSubtitles(RE::StaticFunctionTag*, bool bEnable) {
        SubtitleManager::bSubtitlesEnabled = bEnable;
    }

    // Papyrus function to stop subtitles (and RAW audio, keeping bookmark)
    void StopSubtitles(RE::StaticFunctionTag*) {
        RawAudioManager::Stop(false);
        SubtitleManager::StopSubtitles(-1, false);
    }

    // Papyrus function to reset the bookmark of the current/last book (e.g. when holding Stop key)
    void ResetLastBook(RE::StaticFunctionTag*) {
        // Stop(false): only stop here, the bookmark of the right book is reset below
        RawAudioManager::Stop(false);
        RawAudioManager::ResetBookPosition(lastPlayedBookID);
        SubtitleManager::ResetBookBookmark(lastPlayedBookID);
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
        vm->RegisterFunction("ResetLastBook", "VBoSPapyrusExtensions", ResetLastBook);
        vm->RegisterFunction("IsSubtitleAvailable", "VBoSPapyrusExtensions", IsSubtitleAvailable);
        return true;
    }
}