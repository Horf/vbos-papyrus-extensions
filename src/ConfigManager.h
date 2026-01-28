#pragma once

namespace ConfigManager
{
    void LoadConfigs();
    std::string GetPathForBook(const RE::TESForm* book);
    bool HasMapping(const RE::TESForm* book);
}