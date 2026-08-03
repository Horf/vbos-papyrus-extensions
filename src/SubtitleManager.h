#pragma once

#include <imgui.h>

#include <string>
#include <cstdint>

#include <RE/T/TESObjectBOOK.h>

namespace SubtitleManager
{
    extern bool bSubtitlesEnabled;
    extern float fScrollSpeed;
    extern std::string fontPath;
    extern float fontSize;

    // Parses settings and translations from the INI file
    void LoadSettings();

    // Loads the custom font or falls back to ImGui default
    void LoadFont(ImGuiIO& io);

    // Initiates the subtitle system for a specific book form
    void StartSubtitles(RE::TESObjectBOOK* book, int32_t soundID);

    // Stops and clears currently active subtitles
    void StopSubtitles(int32_t soundID = -1);

    // Pauses or unpauses scrolling text
    void SetPaused(bool isPaused);

    // Main render loop for ImGui subtitles
    void Render();

    // Registers the keyboard input listener for speed adjustments
    void InstallInputSink();

    // Cleans and formats book text by parsing internal tags and aliases
    std::string CleanBookText(std::string rawText);
}