#pragma once

namespace PapyrusInterface
{
    // Helper: Get VM handle for a form
    RE::VMHandle GetHandle(RE::TESForm* akForm);

    // Helper: Send event to specific scripts
    void SendEvents(std::vector<RE::VMHandle> handles, RE::BSScript::IFunctionArguments* args);
    
    // Starts a thread to monitor sound status and fire "OnSoundFinish"
    void CreateSoundEvent(int32_t soundID, std::vector<RE::VMHandle> vmHandles, RE::TESForm* soundForm);

    // Main Function: Plays sound by swapping descriptor file path
    // Returns: InstanceID or -1 on error
    int32_t PlaySound(RE::StaticFunctionTag*, RE::TESObjectBOOK* akBook, RE::BGSSoundDescriptorForm* akTemplateDescriptor, float volume, RE::TESForm* eventReceiverForm);

    // Registration for Papyrus
    bool Register(RE::BSScript::IVirtualMachine* vm);
}