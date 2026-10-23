#include "RawAudioManager.h"

#include <Windows.h>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <string>
#include <system_error>
#include <cstdint>
#include <format>
#include <fstream>
#include <vector>
#include <ios>
#include <chrono>
#include <exception>
#include <algorithm>
#include <array>

#include <REL/Module.h>
#include <SKSE/Logger.h>

#include <RE/B/BSCoreTypes.h>
#include <RE/B/BSResourceNiBinaryStream.h>

namespace RawAudioManager
{
    // Detailed "RAW Audio:" playback lines (start, pause, stop, reset) in the log.
    // Off for releases; set to true and rebuild to analyse bug reports. Warnings and errors are always logged.
    constexpr bool DEBUG_LOG = false;

#define RAW_DEBUG_LOG(...) \
    do { if constexpr (DEBUG_LOG) { logs::info(__VA_ARGS__); } } while (false)

    // Function pointer types matching RAWAudioEngine.dll
    using Fn_AudioInit          = bool (*)();
    using Fn_PlayTrack          = bool (*)(const char*, bool, float, bool, float);
    using Fn_StopTrack          = void (*)(float);
    using Fn_SetVolume          = void (*)(float);
    using Fn_IsPlaying          = bool (*)();
    using Fn_SetResumeEnabled   = void (*)(bool);
    using Fn_ResetTrackPosition = void (*)(const char*);

    namespace
    {
        // Upper limit for files that are copied/extracted for staging (guards against absurdly large files)
        constexpr std::size_t MAX_STAGED_BYTES = 256ull * 1024 * 1024;

        // File name part of a path string without std::filesystem::path
        // (path conversions can throw for characters that do not fit the ANSI code page)
        std::string FileNameOf(const std::string& a_path)
        {
            const auto pos = a_path.find_last_of("/\\");
            return pos == std::string::npos ? a_path : a_path.substr(pos + 1);
        }

        // Oldest RAW Audio Engine version that is known to be stable with VBoS
        constexpr std::array<std::uint16_t, 4> MIN_ENGINE_VERSION{ 1, 4, 0, 0 };

        // Reads the file version of a DLL, returns false if it has none or cannot be read
        bool ReadFileVersion(const std::filesystem::path& a_path, std::array<std::uint16_t, 4>& a_version)
        {
            DWORD handle = 0;
            const DWORD size = GetFileVersionInfoSizeW(a_path.c_str(), &handle);
            if (size == 0) {
                return false;
            }
            std::vector<char> data(size);
            if (!GetFileVersionInfoW(a_path.c_str(), 0, size, data.data())) {
                return false;
            }
            VS_FIXEDFILEINFO* info = nullptr;
            UINT length = 0;
            if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) ||
                !info || length < sizeof(VS_FIXEDFILEINFO)) {
                return false;
            }
            a_version = { static_cast<std::uint16_t>(HIWORD(info->dwFileVersionMS)),
                          static_cast<std::uint16_t>(LOWORD(info->dwFileVersionMS)),
                          static_cast<std::uint16_t>(HIWORD(info->dwFileVersionLS)),
                          static_cast<std::uint16_t>(LOWORD(info->dwFileVersionLS)) };
            return true;
        }

        // Extension for staged files: only plain ASCII letters/digits are accepted, otherwise ".xwm"
        std::string SafeExtension(const std::string& a_path)
        {
            const std::string name = FileNameOf(a_path);
            const auto dot = name.find_last_of('.');
            if (dot == std::string::npos || dot + 1 >= name.size() || name.size() - dot > 8) {
                return ".xwm";
            }
            for (std::size_t i = dot + 1; i < name.size(); ++i) {
                const char c = name[i];
                const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                if (!ok) {
                    return ".xwm";
                }
            }
            return name.substr(dot);
        }

        HMODULE               g_hModule = nullptr;
        Fn_AudioInit          g_Init = nullptr;
        Fn_PlayTrack          g_PlayTrack = nullptr;
        Fn_StopTrack          g_StopTrack = nullptr;
        Fn_SetVolume          g_SetVolume = nullptr;
        Fn_IsPlaying          g_IsPlaying = nullptr;
        Fn_SetResumeEnabled   g_SetResumeEnabled = nullptr;
        Fn_ResetTrackPosition g_ResetTrackPosition = nullptr;

        bool g_available = false;

        std::mutex g_mutex;
        bool g_sessionActive = false;
        bool g_isPaused = false;
        int32_t g_activeSessionID = -1;
        RE::FormID g_currentBookID = 0;
        std::string g_currentPath = "";
        float g_currentGain = 1.0f;

        // Maps Book FormID -> Resolved File Path (so it can reset any book's timestamp)
        std::unordered_map<RE::FormID, std::string> g_bookPathHistory;

        // Books whose bookmark was reset by the player: next playback starts at 0:00 no matter what the engine stored
        std::unordered_set<RE::FormID> g_restartBooks;

        // Tracks the staged audio files (named by content hash) that VBoS created directly in Data
        std::vector<std::filesystem::path> g_tempFiles;

        // 64-bit FNV-1a hash of the audio data
        std::uint64_t HashBytes(const std::vector<char>& a_data)
        {
            std::uint64_t hash = 14695981039346656037ull;
            for (const char c : a_data) {
                hash ^= static_cast<std::uint8_t>(c);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        // Writes audio data to Data/_vbos_temp_<hash><ext> (skipped if that file already exists) and returns the path relative to Data
        std::string StageAudio(const std::vector<char>& a_data, const std::string& a_ext)
        {
            const std::string relPath = std::format("_vbos_temp_{:016x}{}", HashBytes(a_data), a_ext);
            const std::filesystem::path diskPath = std::filesystem::path("Data") / relPath;

            std::error_code ec;
            if (std::filesystem::exists(diskPath, ec) && std::filesystem::file_size(diskPath, ec) == a_data.size()) {
                return relPath;
            }

            std::ofstream outFile(diskPath, std::ios::binary);
            if (!outFile.is_open()) {
                logs::warn("RAW Audio: Could not create temporary file '{}'.", relPath);
                return "";
            }
            outFile.write(a_data.data(), static_cast<std::streamsize>(a_data.size()));
            outFile.flush();
            const bool writeFailed = !outFile.good();
            outFile.close();

            if (writeFailed) {
                std::filesystem::remove(diskPath, ec);
                logs::warn("RAW Audio: Failed to write temporary file '{}' (disk full?).", relPath);
                return "";
            }

            g_tempFiles.push_back(diskPath);
            return relPath;
        }

        // Deletes staged files from Data
        void CleanupBSACache(const std::string& a_keepFileName = {})
        {
            const std::string keepActive = g_sessionActive ? FileNameOf(g_currentPath) : std::string{};

            std::error_code ec;
            std::erase_if(g_tempFiles, [&](const std::filesystem::path& tempPath) {
                const std::string name = tempPath.filename().string();
                if ((!a_keepFileName.empty() && name == a_keepFileName) ||
                    (!keepActive.empty() && name == keepActive)) {
                    return false;
                }
                if (!std::filesystem::exists(tempPath, ec)) {
                    return true;
                }
                return std::filesystem::remove(tempPath, ec);
            });
        }

        // Extracts an audio file from a Skyrim BSA into a temporary file in Data 
        std::string ExtractFromBSAToCache(const std::string& a_relativePath, bool a_logNotFound = true)
        {
            const std::string ext = SafeExtension(a_relativePath);

            // Open stream from Skyrim's BSA / resource system
            RE::BSResourceNiBinaryStream bsaStream(a_relativePath);
            if (!bsaStream.good() || !bsaStream.stream) {
                if (a_logNotFound) {
                    logs::warn("RAW Audio: Could not find '{}' in BSAs.", a_relativePath);
                }
                return "";
            }

            // Read the whole stream (book audio is small): the hash of the content decides the file name
            std::vector<char> data;
            std::vector<char> buffer(262144);
            std::uint64_t bytesRead = 0;
            do {
                bytesRead = 0;
                bsaStream.stream->DoRead(buffer.data(), buffer.size(), bytesRead);
                if (bytesRead > 0) {
                    data.insert(data.end(), buffer.data(), buffer.data() + static_cast<std::size_t>(bytesRead));
                    if (data.size() > MAX_STAGED_BYTES) {
                        logs::warn("RAW Audio: '{}' is too large to stage. Falling back to vanilla audio.", a_relativePath);
                        return "";
                    }
                }
            } while (bytesRead > 0);

            if (data.empty()) {
                return "";
            }
            return StageAudio(data, ext);
        }
    }

    void Initialize()
    {
        // Clean up any leftover _vbos_temp_ files from a previous session
        std::error_code ec;
        try {
            if (std::filesystem::exists("Data", ec)) {
                for (const auto& entry : std::filesystem::directory_iterator("Data", ec)) {
                    if (entry.path().filename().native().starts_with(L"_vbos_temp_") && entry.is_regular_file(ec)) {
                        std::filesystem::remove(entry.path(), ec);
                    }
                }
            }
        }
        catch (const std::exception& e) {
            logs::warn("RAW Audio: Cleanup of leftover temporary files failed: {}", e.what());
        }

        // RAW Audio Engine does not support Skyrim VR yet
        if (REL::Module::IsVR()) {
            logs::info("RAW Audio Engine integration skipped (VR mode detected).");
            return;
        }

        const std::filesystem::path dllPath{ "Data/SKSE/Plugins/RAWAudioEngine.dll" };
        if (!std::filesystem::exists(dllPath, ec)) {
            logs::info("RAWAudioEngine.dll not found. Using default Skyrim audio engine.");
            return;
        }

        std::array<std::uint16_t, 4> engineVersion{};
        if (!ReadFileVersion(dllPath, engineVersion)) {
            logs::warn("Found RAWAudioEngine.dll, but could not read its version. Using default Skyrim audio engine.");
            return;
        }
        if (engineVersion < MIN_ENGINE_VERSION) {
            logs::warn("RAWAudioEngine.dll {}.{}.{}.{} is too old (needs {}.{}.{}.{} or newer). Using default Skyrim audio engine.",
                engineVersion[0], engineVersion[1], engineVersion[2], engineVersion[3],
                MIN_ENGINE_VERSION[0], MIN_ENGINE_VERSION[1], MIN_ENGINE_VERSION[2], MIN_ENGINE_VERSION[3]);
            return;
        }
        g_hModule = LoadLibraryA(dllPath.string().c_str());

        if (!g_hModule) {
            logs::warn("Found RAWAudioEngine.dll, but failed to load it.");
            return;
        }

        g_Init               = reinterpret_cast<Fn_AudioInit>(GetProcAddress(g_hModule, "RawAudioCore_Initialize"));
        g_PlayTrack          = reinterpret_cast<Fn_PlayTrack>(GetProcAddress(g_hModule, "RawAudioCore_PlayTrack"));
        g_StopTrack          = reinterpret_cast<Fn_StopTrack>(GetProcAddress(g_hModule, "RawAudioCore_StopCurrentTrack"));
        g_SetVolume          = reinterpret_cast<Fn_SetVolume>(GetProcAddress(g_hModule, "RawAudioCore_SetVolume"));
        g_IsPlaying          = reinterpret_cast<Fn_IsPlaying>(GetProcAddress(g_hModule, "RawAudioCore_IsPlaying"));
        g_SetResumeEnabled   = reinterpret_cast<Fn_SetResumeEnabled>(GetProcAddress(g_hModule, "RawAudioCore_SetResumePositionEnabled"));
        g_ResetTrackPosition = reinterpret_cast<Fn_ResetTrackPosition>(GetProcAddress(g_hModule, "RawAudioCore_ResetTrackPosition"));

        if (!g_PlayTrack || !g_StopTrack || !g_IsPlaying || !g_ResetTrackPosition) {
            logs::error("RAWAudioEngine.dll is missing required exports.");
            return;
        }

        if (g_Init && !g_Init()) {
            logs::info("RAW Audio: Core initialization deferred (audio device is initialized lazily).");
        }
        if (g_SetResumeEnabled) {
            g_SetResumeEnabled(true);
        }

        g_available = true;
        logs::info("Connected to RAWAudioEngine.dll");
    }

    bool IsAvailable()
    {
        return g_available;
    }

    std::string ResolveAudioPath(const std::string& a_relativePath)
    {
        if (!g_available || a_relativePath.empty()) {
            return "";
        }

        // Paths with non-ASCII bytes cannot be resolved reliably, so those books use the Skyrim audio engine
        if (std::ranges::any_of(a_relativePath, [](unsigned char c) { return c >= 0x80; })) {
            logs::info("RAW Audio: '{}' contains non-ASCII characters, using the Skyrim audio engine.", a_relativePath);
            return "";
        }

        // g_tempFiles is shared with Play(), so the whole lookup runs under the mutex
        std::scoped_lock lock(g_mutex);

        // Every result goes through here: staged files of other books are removed, the one about to be played is kept
        auto finish = [](std::string a_result) {
            CleanupBSACache(std::filesystem::path(a_result).filename().string());
            return a_result;
        };

        try {
            std::error_code ec;

            // Build the equivalent .wav path (e.g. "sound\...\book.xwm" -> "sound\...\book.wav") with plain string operations
            std::string wavRelativePath = a_relativePath;
            const auto lastSep = wavRelativePath.find_last_of("/\\");
            const auto lastDot = wavRelativePath.find_last_of('.');
            if (lastDot != std::string::npos && (lastSep == std::string::npos || lastDot > lastSep)) {
                wavRelativePath.erase(lastDot);
            }
            wavRelativePath += ".wav";

            // Loose .wav in Data: the engine plays it directly (no conversion, no cache)
            if (std::filesystem::exists(std::filesystem::path("Data") / wavRelativePath, ec)) {
                return finish(wavRelativePath);
            }

            // .wav inside BSAs
            const std::string stagedWav = ExtractFromBSAToCache(wavRelativePath, false);
            if (!stagedWav.empty()) {
                return finish(stagedWav);
            }

            // Loose .xwm in Data: handed to the engine as it is (RAW Audio Engine 1.4.0.0+ names its cache by content hash)
            if (std::filesystem::exists(std::filesystem::path("Data") / a_relativePath, ec)) {
                return finish(a_relativePath);
            }

            // .xwm inside BSAs
            return finish(ExtractFromBSAToCache(a_relativePath, true));
        }
        catch (const std::exception& e) {
            logs::warn("RAW Audio: Resolving '{}' failed ({}). Falling back to vanilla audio.", a_relativePath, e.what());
            return "";
        }
    }

    bool Play(RE::FormID a_bookID, const std::string& a_resolvedPath, float a_gain, int32_t a_sessionID)
    {
        std::scoped_lock lock(g_mutex);
        if (!g_available || a_resolvedPath.empty()) {
            return false;
        }

        // Ensure resume is enabled before playing
        if (g_SetResumeEnabled) {
            g_SetResumeEnabled(true);
        }

        // Prepend "Data/" and use forward slashes so RawAudioEngine doesn't default to "Data/Music/"
        std::string enginePath = "Data/" + a_resolvedPath;
        for (char& c : enginePath) {
            if (c == '\\') c = '/';
        }

        bool engineStarted = false;
        try {
            std::error_code sizeEc;
            const auto fileSize = std::filesystem::file_size(enginePath, sizeEc);
            const auto startTime = std::chrono::steady_clock::now();

            // ShouldLoop = false; resumePosition = true, except right after a manual bookmark reset
            const bool resume = g_restartBooks.erase(a_bookID) == 0;
            bool started = g_PlayTrack(enginePath.c_str(), false, 0.0f, resume, a_gain);
            engineStarted = started;

            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();

            if (started) {
                g_currentBookID = a_bookID;
                g_currentPath = enginePath;
                g_currentGain = a_gain;
                g_activeSessionID = a_sessionID;
                g_sessionActive = true;
                g_isPaused = false;
                g_bookPathHistory[a_bookID] = enginePath;
                RAW_DEBUG_LOG("RAW Audio: Playing '{}' (Book 0x{:X}, Gain {:.2f}, resume = {}, started in {} ms)", enginePath, a_bookID, a_gain, resume, elapsedMs);
                return true;
            }

            // PlayTrack failed -> a pending bookmark reset must survive for the next attempt
            if (!resume) {
                g_restartBooks.insert(a_bookID);
            }

            // PlayTrack failed -> immediately delete the temporary file
            CleanupBSACache();
            logs::warn("RAW Audio: PlayTrack returned false for '{}' (size {} bytes, {} ms). Falling back to vanilla audio.",
                enginePath, sizeEc ? 0 : fileSize, elapsedMs);
            return false;
        }
        catch (const std::exception& e) {
            logs::error("RAW Audio: Playback failed with an exception ({}). Falling back to vanilla audio.", e.what());
            if (engineStarted && g_StopTrack) {
                g_StopTrack(0.0f);
            }
            g_sessionActive = false;
            g_isPaused = false;
            g_activeSessionID = -1;
            CleanupBSACache();
            return false;
        }
    }

    bool Stop(bool a_resetPosition)
    {
        std::scoped_lock lock(g_mutex);
        if (!g_available) {
            return false;
        }

        const bool wasRunning = g_sessionActive && !g_isPaused;

        if (g_sessionActive) {
            g_StopTrack(0.0f);
            g_sessionActive = false;
            g_isPaused = false;
            g_activeSessionID = -1;
            // the staged file is not needed anymore (it is extracted again when the book is reopened)
            CleanupBSACache();
            RAW_DEBUG_LOG("RAW Audio: Stopped playback (bookmark kept = {}).", !a_resetPosition);
        }

        if (a_resetPosition && !g_currentPath.empty() && g_ResetTrackPosition) {
            g_ResetTrackPosition(g_currentPath.c_str());
        }
        if (a_resetPosition && g_currentBookID != 0) {
            g_restartBooks.insert(g_currentBookID);
        }

		return wasRunning;
    }

    bool FinishSession(int32_t a_sessionID, bool a_resetPosition)
    {
        std::scoped_lock lock(g_mutex);
        // A new book might have been started in the meantime -> do not touch its session
        if (!g_available || !g_sessionActive || g_activeSessionID != a_sessionID) {
            return false;
        }

        g_StopTrack(0.0f);
        g_sessionActive = false;
        g_isPaused = false;
        g_activeSessionID = -1;
        CleanupBSACache();

        if (a_resetPosition && !g_currentPath.empty() && g_ResetTrackPosition) {
            g_ResetTrackPosition(g_currentPath.c_str());
        }
        if (a_resetPosition && g_currentBookID != 0) {
            // the next playback starts at 0:00 for sure
            g_restartBooks.insert(g_currentBookID);
        }
        RAW_DEBUG_LOG("RAW Audio: Session finished (bookmark reset = {}).", a_resetPosition);
        return true;
    }

    void SetPaused(bool a_paused)
    {
        std::scoped_lock lock(g_mutex);
        if (!g_available || !g_sessionActive || g_currentPath.empty()) {
            return;
        }

        if (a_paused && !g_isPaused) {
            // Stopping the track in RAW Audio Engine automatically stores the exact timestamp
            g_StopTrack(0.0f);
            g_isPaused = true;
            RAW_DEBUG_LOG("RAW Audio: Paused playback.");
        }
        else if (!a_paused && g_isPaused) {
            // Re-playing with resumePosition = true continues right where it stopped
            if (g_PlayTrack(g_currentPath.c_str(), false, 0.0f, true, g_currentGain)) {
                g_isPaused = false;
                RAW_DEBUG_LOG("RAW Audio: Resumed playback.");
            }
            else {
                // Without this the monitoring thread would wait forever for a track that never starts
                logs::warn("RAW Audio: Resume failed for '{}'. Ending session.", g_currentPath);
                g_sessionActive = false;
                g_isPaused = false;
                g_activeSessionID = -1;
                CleanupBSACache();
            }
        }
    }

    void SetVolume(float a_gain)
    {
        std::scoped_lock lock(g_mutex);
        g_currentGain = a_gain;
        if (!g_available || !g_sessionActive || g_isPaused) {
            return;
        }
        if (g_SetVolume) {
            g_SetVolume(a_gain);
        }
    }

    void ResetBookPosition(RE::FormID a_bookID)
    {
        std::scoped_lock lock(g_mutex);
        if (!g_available || !g_ResetTrackPosition) {
            return;
        }

        RE::FormID targetID = (a_bookID != 0) ? a_bookID : g_currentBookID;
        if (targetID == 0) {
            return;
        }

        auto it = g_bookPathHistory.find(targetID);
        if (it != g_bookPathHistory.end() && !it->second.empty()) {
            g_ResetTrackPosition(it->second.c_str());
            g_restartBooks.insert(targetID);
            RAW_DEBUG_LOG("Reset audio bookmark for Book 0x{:X}", targetID);
        }
    }

    bool IsPlaying()
    {
        std::scoped_lock lock(g_mutex);
        if (!g_available || !g_sessionActive) {
            return false;
        }
        if (g_isPaused) {
            // While paused, the session is still alive so the monitoring thread waits
            return true;
        }
        return g_IsPlaying ? g_IsPlaying() : false;
    }

    bool IsSessionActive(int32_t a_sessionID)
    {
        std::scoped_lock lock(g_mutex);
        if (!g_sessionActive) {
            return false;
        }
        return (a_sessionID == -1) || (g_activeSessionID == a_sessionID);
    }
}