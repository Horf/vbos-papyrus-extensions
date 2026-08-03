#pragma once

#include <string>
#include <RE/T/TESForm.h>

namespace ConfigManager
{
    // Scans Data folder and loads all valid _VBOS.ini files
    void LoadConfigs();

    // Returns the assigned audio file path for a given book, or empty string if none
    std::string GetPathForBook(const RE::TESForm* book);

    // Checks if a book form is present in the configuration map
    bool HasMapping(const RE::TESForm* book);
}