#pragma once

#include <string>
#include <cstdint>

#include <RE/B/BSCoreTypes.h>

namespace RawAudioManager
{
    // Connects to RawAudioEngine.dll if installed (skipped on VR for now)
    void Initialize();

    // Returns true if RawAudioEngine.dll is loaded and ready
    bool IsAvailable();

    // Checks if the audio file is available for RAW playback (loose file or cached)
    std::string ResolveAudioPath(const std::string& a_relativePath);

    // Starts or resumes playback of a book track
    bool Play(RE::FormID a_bookID, const std::string& a_resolvedPath, float a_gain, int32_t a_sessionID);

    // Stops playback (keeps timestamp in memory unless a_resetPosition is true)
    // Returns true if audio was actually running (not paused) when it was stopped
    bool Stop(bool a_resetPosition = false);

    // Ends the session only if a_sessionID is still the active one (check + stop happen atomically).
    // Returns true if the session was ended by this call.
    bool FinishSession(int32_t a_sessionID, bool a_resetPosition);

    // Pauses or unpauses the currently active RAW book track
    void SetPaused(bool a_paused);

    // Updates the volume of the currently playing track
    void SetVolume(float a_gain);

    // Resets the saved timestamp for a specific book (or the last played book)
    void ResetBookPosition(RE::FormID a_bookID = 0);

    // Returns true if the RAW engine is currently streaming audio
    bool IsPlaying();

    // Returns true if VBoS is currently managing an active RAW session (playing or paused)
    bool IsSessionActive(int32_t a_sessionID = -1);
}