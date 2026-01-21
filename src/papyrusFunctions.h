#pragma once

namespace papyrusFunctions
{
    // Functions //

    // Retrieves the Virtual Machine Handle for a given form.
    // Necessary to send events back to specific scripts attached to forms/aliases.
    RE::VMHandle GetHandle(RE::TESForm* akForm);

    // Relays a named event (e.g., "OnSoundFinish") to the specified VM handles.
    void SendEvents(std::vector<RE::VMHandle> handles, RE::BSScript::IFunctionArguments* args);
    
    // Starts a detached thread that monitors the sound status and triggers the event when finished.
    void CreateSoundEvent(int soundID, std::vector<RE::VMHandle> vmHandles, RE::TESForm* soundForm);


    // Papyrus function signatures //

    // Plays a sound descriptor. 
    // If no source is provided (handled internally), it plays as a 2D UI sound (ideal for voiceovers).
    // Returns the unique SoundID (or -1 on failure).
    int32_t PlaySound(RE::StaticFunctionTag*, RE::TESSound* akSound, float volume, RE::TESForm* eventReceiverForm);
    
    // Helper to retrieve the sound category (e.g., Master, Voice, Effects) from a descriptor.
    RE::BGSSoundCategory* GetSoundCategoryForSoundDescriptor(RE::StaticFunctionTag*, RE::BGSSoundDescriptorForm* akSoundDescriptor);

    // Registers these functions to be usable within Papyrus scripts.
    bool Register(RE::BSScript::IVirtualMachine* vm);
}