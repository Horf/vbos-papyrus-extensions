#include "ConfigManager.h"

namespace ConfigManager
{
    // Maps Book: FormID -> Sound File Path
    std::unordered_map<RE::FormID, std::string> bookToPathMap;
    
    // Metadata Struct for logging
    struct ConfigEntryMeta {
        std::string fileName;
        int lineNumber = 0;
        std::string fullPath;
    };

    std::string_view Trim(std::string_view str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (std::string_view::npos == first) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    // Fix path separators and remove 'data' prefix for Skyrim engine
    std::string NormalizePath(std::string_view strPath) {
        std::string path(strPath);
        for (char& c : path) {
            if (c == '/') c = '\\';
            else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (path.starts_with("data\\")) return path.substr(5);
        return path;
    }

    void LoadConfigs() {
        bookToPathMap.clear();

		// Early exit if neither data handler or...
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            logs::critical("Skyrim DataHandler not found!");
            return;
        }
		// ...data directory is found
        const std::filesystem::path configPath{ "Data" };
        std::error_code ec;
        if (!std::filesystem::exists(configPath, ec)) {
            std::string ecMessage = ec.message();
            logs::critical("Data directory not found or not accessible! Error: {}", ecMessage);
            return;
        }

		// Temp map for Metadata (for conflict logging)
        std::unordered_map<RE::FormID, ConfigEntryMeta> metadataMap;

        size_t globalTotal = 0;
        size_t globalOverwrites = 0;
        size_t globalErrors = 0;

		// Security check, in case of filesystem errors
        std::error_code iteratorError;
        auto directoryIterator = std::filesystem::directory_iterator(configPath, iteratorError);
        if (iteratorError) {
            std::string iteratorMessage = iteratorError.message();
            logs::error("Failed to begin directory scan: {}", iteratorMessage);
            return;
        }

        // Find all _VBOS.ini files
        std::vector<std::filesystem::path> validFiles;
        std::vector<std::string> ignoredFiles;
        for (const auto& entry : directoryIterator) {
            std::error_code entryError;
            if (entry.is_regular_file(entryError)) {
                auto currentPath = entry.path();

                // Safe conversion from std::filesystem::path to UTF-8 std::string
                auto u8Name = currentPath.filename().u8string();
                std::string fileName(reinterpret_cast<const char*>(u8Name.c_str()));
                if (fileName.empty()) continue;

                // Case-insensitive check for suffix to find valid config files for VBoS
                std::string fileNameLower = fileName;
                std::ranges::transform(fileNameLower, fileNameLower.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (fileNameLower.ends_with("_vbos.ini")) {
                    validFiles.push_back(currentPath);
                }
                else if (fileNameLower.find("vbos") != std::string::npos && fileNameLower.ends_with(".ini")) {
                    ignoredFiles.push_back(fileName);
                }
            }
            // Ignore non-regular files
            else continue;
        }

        std::ranges::sort(validFiles);
        std::ranges::sort(ignoredFiles);

        if (!ignoredFiles.empty()) {
            logs::info("==========================================\n"
                "\t\t\t\tIgnored configuration Files...\n"
                "\t\t\t   ==========================================");
            for (const auto& fileName : ignoredFiles) {
                logs::info("{} has wrong suffix, has to be: '_VBOS.ini'\n", fileName);
            }
        }

		// No valid files are found
        if (validFiles.empty()) {
            logs::warn("====================================================================================\n"
                "\t\t\t\t\t\t\tVBoS WARNING: No valid configuration files found!\n"
                "\t\t\t\tThe mod will not function without a valid '*_VBOS.ini' file in the Data folder.\n"
                "\t\t\t\t\tPossible causes: Wrong installation or unreadable filename characters.\n"
                "\t\t\t   ====================================================================================");
            return;
        }

        logs::info("==========================================\n"
            "\t\t\t\tLoading configuration Files...\n"
            "\t\t\t   ==========================================");

        for (const auto& filePath : validFiles) {
            std::ifstream file(filePath);
            if (!file.is_open()) continue;

            std::string fileName = filePath.filename().string();
            logs::info("Processing config: {}...", fileName);

            std::string line;
            int lineNum = 0;
            bool isFirstLine = true;

            // File Stats
            size_t fileSwaps = 0;
            size_t fileOverwrites = 0;
            size_t fileErrors = 0;

            while (std::getline(file, line)) {
                lineNum++;
                // BOM check
                if (isFirstLine) {
                    isFirstLine = false;
                    if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
                }

                std::string_view trimmedLine = Trim(line);
                if (trimmedLine.empty() || trimmedLine[0] == ';') continue;

                // --- Parsing Start ---
                
                // Parse: FormID~Plugin | Path
                auto pipePos = trimmedLine.find('|');
                if (pipePos == std::string_view::npos) {
                    logs::warn("\tFail: Line {} - Missing pipe '|' separator", lineNum);
                    fileErrors++; continue;
                }

                auto fullId = Trim(trimmedLine.substr(0, pipePos));
                auto rawPath = Trim(trimmedLine.substr(pipePos + 1));

                if (rawPath.empty()) {
                    logs::warn("\tFail: Line {} - Sound file path is empty", lineNum);
                    fileErrors++; continue;
                }

                auto tildePos = fullId.find('~');
                if (tildePos == std::string_view::npos) {
                    logs::warn("\tFail: Line {} - Missing tilde '~' separator in ID", lineNum);
                    fileErrors++; continue;
                }

                auto formIdHex = Trim(fullId.substr(0, tildePos));
                std::string pluginName(Trim(fullId.substr(tildePos + 1)));

                // Check: Empty inputs
                if (formIdHex.empty()) {
                    logs::warn("\tFail: Line {} - FormID is missing before '~'", lineNum);
                    fileErrors++; continue;
                }
                if (pluginName.empty()) {
                    logs::warn("\tFail: Line {} - Plugin name is missing after '~'", lineNum);
                    fileErrors++; continue;
                }
                // Check: 0x Prefix
                if (formIdHex.starts_with("0x") || formIdHex.starts_with("0X")) {
                    formIdHex.remove_prefix(2);
                }

				// Convert Hex String to FormID and push to map
                RE::FormID localID = 0;
                auto res = std::from_chars(formIdHex.data(), formIdHex.data() + formIdHex.size(), localID, 16);
                if (res.ec != std::errc() || res.ptr != (formIdHex.data() + formIdHex.size())) {
                    logs::warn("\tFail: Line {} - Invalid Hex ID format: 0x{}", lineNum, formIdHex);
                    fileErrors++; continue;
                }
				// Lookup Form
                auto* bookForm = dataHandler->LookupForm(localID, pluginName);
				// Combined Check: Not found OR Not a Book
                if (!bookForm || bookForm->GetFormType() != RE::FormType::Book) {
                    logs::warn("\tFail: Line {} - FormID 0x{:X} in '{}' is NOT a valid Book ", lineNum, localID, pluginName);
                    fileErrors++; continue;
                }

                RE::FormID globalID = bookForm->GetFormID();
                std::string cleanPath = NormalizePath(rawPath);

                // --- Conflict Logic ---

                if (metadataMap.contains(globalID)) {
					// Entry already exists
                    const auto& oldEntry = metadataMap[globalID];
                    bool isInternal = (oldEntry.fileName == fileName);

                    if (isInternal) {
                        logs::warn(
                            "\tConflict: Line {} - Internal overwrite for Book: 0x{:X}\n"
                            "\t\t\t\t\tLosing:  Line {} -> {}\n"
                            "\t\t\t\t\tWinning: Line {} -> {}",
                            lineNum, globalID,
                            oldEntry.lineNumber, oldEntry.fullPath,
                            lineNum, cleanPath
                        );
                    }
                    else {
                        logs::warn(
                            "\tConflict: Line {} - External overwrite for Book: 0x{:X}\n"
                            "\t\t\t\t\tLosing:  Line {} in {} -> {}\n"
                            "\t\t\t\t\tWinning: Line {} in {} -> {}",
                            lineNum, globalID,
                            oldEntry.lineNumber, oldEntry.fileName, oldEntry.fullPath,
                            lineNum, fileName, cleanPath
                        );
                    }
                    fileOverwrites++;
                }
                else fileSwaps++;
                // Update Metadata & Runtime Map
                metadataMap[globalID] = { fileName, lineNum, cleanPath };
                bookToPathMap[globalID] = cleanPath;
            }

            // Add file stats to global stats
            globalErrors += fileErrors;
            globalOverwrites += fileOverwrites;
            globalTotal += fileSwaps;

            // Summary per File
            if (fileErrors == 0 && fileOverwrites == 0 && fileSwaps > 0) {
                logs::info("\tFile read without errors ({} mappings found)\n", fileSwaps);
            }
            else {
                logs::info("\tResult:\n"
                    "\t\t\t\t\t{} new mappings\n"
                    "\t\t\t\t\t{} overwrites\n"
                    "\t\t\t\t\t{} errors\n",
                    fileSwaps,
                    fileOverwrites,
                    fileErrors);
            }
        }

        logs::info("==========================================\n"
            "\t\t\t\tLoading complete\n"
            "\t\t\t\t\tTotal Active Mappings : {}\n"
            "\t\t\t\t\tTotal Overwrites : {}\n"
            "\t\t\t\t\tTotal Errors : {}",
            bookToPathMap.size(),
            globalOverwrites,
            globalErrors);
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