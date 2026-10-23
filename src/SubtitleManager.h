#pragma once

#include <imgui.h>

#include <string>
#include <cstdint>

#include <RE/B/BSCoreTypes.h>
#include <RE/T/TESObjectBOOK.h>

namespace SubtitleManager
{
    extern bool bSubtitlesEnabled;
    extern float fScrollSpeed;
    extern std::string fontPath;
    extern float fontSize;

    // Bounding Box Settings
    extern float fWindowWidthMult;
    extern float fWindowHeightMult;
    extern float fWindowBottomOffset;

    // Parses settings and translations from the INI file
    void LoadSettings();

    // Loads the custom font or falls back to ImGui default
    void LoadFont(ImGuiIO& io);

    // Initiates the subtitle system for a specific book form
    void StartSubtitles(RE::TESObjectBOOK* book, int32_t soundID, bool useBookmark = false);

    // Stops and clears currently active subtitles
    void StopSubtitles(int32_t soundID = -1, bool resetBookmark = false);

    // Resets the saved subtitle scroll position for a book (or the last read book)
    void ResetBookBookmark(RE::FormID bookID = 0);

    // Pauses or unpauses scrolling text
    void SetPaused(bool isPaused);

    // Main render loop for ImGui subtitles
    void Render();

    // Registers the keyboard input listener for speed adjustments
    void InstallInputSink();

    // Moves the saved position of the last read book back by a number of seconds
    void RewindBookBookmark(float a_seconds);

    // Cleans and formats book text by parsing internal tags and aliases
    std::string CleanBookText(std::string rawText);
}