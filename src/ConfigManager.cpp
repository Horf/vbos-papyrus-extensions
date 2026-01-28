#include "ConfigManager.h"

namespace ConfigManager
{
    // Maps Book FormID -> Sound File Path
    std::unordered_map<RE::FormID, std::string> bookToPathMap;

    std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (std::string::npos == first) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    // Fix path separators and remove 'data' prefix for Skyrim engine
    std::string NormalizePath(const std::string_view strPath) {
        std::filesystem::path p(strPath);
        std::string path = p.make_preferred().string();

        std::ranges::transform(path, path.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        if (path.starts_with("data\\")) {
            return path.substr(5);
        }
        return path;
    }

    void LoadConfigs() {
        bookToPathMap.clear();
        size_t totalEntries = 0;

        // Base configfile path
        const std::filesystem::path configPath{ "Data" };

		// Security check, but honestly shouldn't be necessary
        if (!std::filesystem::exists(configPath)) {
            logs::error("Data folder not found!");
            return;
        }

        // Find all _VBOS.ini files
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(configPath)) {
            if (entry.is_regular_file()) {
                auto currentPath = entry.path();
                if (currentPath.filename().string().ends_with("_VBOS.ini")) {
                    files.push_back(currentPath);
                }
            }
        }
        std::ranges::sort(files);

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return;

        for (const auto& filePath : files) {
            std::ifstream file(filePath);
            if (!file.is_open()) continue;

            std::string fileName = filePath.filename().string();
            logs::info("Processing file: {}", fileName);

            std::string line;
            int lineNum = 0;
            bool isFirstLine = true;

            while (std::getline(file, line)) {
                lineNum++;
                // BOM check
                if (isFirstLine) {
                    isFirstLine = false;
                    if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
                }

                std::string trimmedLine = Trim(line);
                if (trimmedLine.empty() || trimmedLine[0] == ';') continue;

                // Parse: FormID~Plugin | Path
                auto pipePos = trimmedLine.find('|');
                if (pipePos == std::string::npos) {
                    logs::warn("Line {}: Missing pipe '|' separator.", lineNum);
                    continue;
                }

                totalEntries++;

                std::string fullId = Trim(trimmedLine.substr(0, pipePos));
                std::string rawPath = Trim(trimmedLine.substr(pipePos + 1));

                auto tildePos = fullId.find('~');
                if (tildePos == std::string::npos) {
                    logs::warn("Line {}: Missing tilde '~' separator in ID.", lineNum);
                    continue;
                }

                std::string formIdStr = Trim(fullId.substr(0, tildePos));
                std::string pluginName = Trim(fullId.substr(tildePos + 1));

				// Convert Hex String to FormID and push to map
                try {
                    RE::FormID localID = std::stoul(formIdStr, nullptr, 16);
                    if (auto* bookForm = dataHandler->LookupForm(localID, pluginName)) {
                        bookToPathMap[bookForm->GetFormID()] = NormalizePath(rawPath);
                    }
                    else logs::warn("Line {}: FormID {:X} in {} not found!", lineNum, localID, pluginName);
                }
                catch (const std::invalid_argument&) {
                    logs::warn("Line {}: Invalid Hex ID format '{}'. Skipping.", lineNum, formIdStr);
                }
                catch (const std::out_of_range&) {
                    logs::warn("Line {}: Hex ID out of range '{}'. Skipping.", lineNum, formIdStr);
                }
            }
        }
        logs::info("ConfigManager: Total {}/{} mappings loaded.", bookToPathMap.size(), totalEntries);
    }

    std::string GetPathForBook(const RE::TESForm* book) {
        if (!book) return "";
        auto it = bookToPathMap.find(book->GetFormID());
        return (it != bookToPathMap.end()) ? it->second : "";
    }

    bool HasMapping(const RE::TESForm* book) {
        return book && bookToPathMap.contains(book->GetFormID());
    }
}