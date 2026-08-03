#include "SubtitleManager.h"
#include "RenderManager.h"

#include <imgui.h>

#include <regex>
#include <string>
#include <fstream>
#include <stdexcept>
#include <cfloat>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <format>
#include <cstdint>
#include <vector>

#include <RE/B/BSInputDeviceManager.h>
#include <RE/B/BSString.h>
#include <RE/B/BSTEvent.h>
#include <RE/B/ButtonEvent.h>
#include <RE/I/InputDevices.h>
#include <RE/I/InputEvent.h>
#include <RE/T/TESObjectBOOK.h>
#include <RE/U/UI.h>

#include <SKSE/Logger.h>

namespace SubtitleManager
{
    // Global Settings
    bool bSubtitlesEnabled = false;
    float fScrollSpeed = 12.5f;
    std::string fontPath = "";
    float fontSize = 24.0f;
    ImVec4 textColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    // Keyboard controls
    uint32_t keySpeedUp = 78;   // Num+
    uint32_t keySpeedDown = 74; // Num-
    uint32_t keyModifier = 56;  // Left Alt
    bool isModifierPressed = false;

    // Runtime state
    bool isReading = false;
    bool isPaused = false;
    std::string currentText = "";
    float currentScrollY = 0.0f;
    int32_t activeSoundID = -1;

	// Alias translations as default fallback
    std::string t_Player = "Friend";
    std::string t_Spouse = "Spouse";
    std::string t_Child = "Child";

    std::string t_Killer = "Killer";
    std::string t_Enemy = "Enemy";
    std::string t_Thief = "Thief";
    std::string t_Boss = "the leader";

    std::string t_Victim = "the victim";
    std::string t_Dead = "the deceased";
    std::string t_Target = "the target";

    std::string t_Jarl = "the Jarl";
    std::string t_Steward = "the Steward";
    std::string t_Client = "the Client";

    std::string t_City = "this city";
    std::string t_Hold = "this hold";
    std::string t_Loc = "this place";

    std::string t_Item = "this item";
    std::string t_Book = "this book";

    std::string t_RaceGen = "unknown";
    std::string t_Pro = "This person";
    std::string t_ProObj = "this person";
    std::string t_ProPosObj = "this person's";
    std::string t_Date = "this day";
    std::string t_Money = "some";

    std::string t_FallbackPerson = "Someone";
    std::string t_FallbackNumber = "some";

    // Helper: Removes leading and trailing whitespace
    static std::string TrimStr(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    void LoadSettings() {
        std::ifstream file("Data\\SKSE\\Plugins\\VBoS_Subtitles.ini");
        if (!file.is_open()) {
            logs::info("No VBoS_Subtitles.ini found. Using default subtitle settings.");
            return;
        }

        std::string line;
        float r = 255.0f, g = 255.0f, b = 255.0f;

        while (std::getline(file, line)) {
            auto delim = line.find('=');
            if (delim != std::string::npos) {
                std::string key = TrimStr(line.substr(0, delim));
                std::string val = TrimStr(line.substr(delim + 1));

                try {
                    // Settings
                    if (key == "ScrollSpeed") {
                        fScrollSpeed = std::stof(val);
                        if (fScrollSpeed < 0.0f) {
                            fScrollSpeed = 12.5f;
                            logs::info("Negative ScrollSpeed found in INI. Resetting to default (12.5).");
                        }
                    }
                    else if (key == "FontPath") fontPath = val;
                    else if (key == "FontSize") fontSize = std::stof(val);
                    else if (key == "ColorR") r = std::clamp(std::stof(val), 0.0f, 255.0f);
                    else if (key == "ColorG") g = std::clamp(std::stof(val), 0.0f, 255.0f);
                    else if (key == "ColorB") b = std::clamp(std::stof(val), 0.0f, 255.0f);
                    else if (key == "KeySpeedUp") keySpeedUp = std::stoul(val);
                    else if (key == "KeySpeedDown") keySpeedDown = std::stoul(val);
                    else if (key == "KeyModifier") keyModifier = std::stoul(val);

					// Translations
                    else if (key == "AliasPlayer") t_Player = val;
                    else if (key == "AliasSpouse") t_Spouse = val;
                    else if (key == "AliasChild") t_Child = val;

                    else if (key == "AliasKiller") t_Killer = val;
                    else if (key == "AliasEnemy") t_Enemy = val;
                    else if (key == "AliasThief") t_Thief = val;
                    else if (key == "AliasBoss") t_Boss = val;

                    else if (key == "AliasVictim") t_Victim = val;
                    else if (key == "AliasDead") t_Dead = val;
                    else if (key == "AliasTarget") t_Target = val;

                    else if (key == "AliasJarl") t_Jarl = val;
                    else if (key == "AliasSteward") t_Steward = val;
                    else if (key == "AliasClient") t_Client = val;

                    else if (key == "AliasCity") t_City = val;
                    else if (key == "AliasHold") t_Hold = val;
                    else if (key == "AliasLoc") t_Loc = val;

                    else if (key == "AliasItem") t_Item = val;
                    else if (key == "AliasBook") t_Book = val;

                    else if (key == "AliasRaceGen") t_RaceGen = val;
                    else if (key == "AliasPro") t_Pro = val;
                    else if (key == "AliasProObj") t_ProObj = val;
                    else if (key == "AliasProPosObj") t_ProPosObj = val;

                    else if (key == "GlobalDate") t_Date = val;
                    else if (key == "GlobalMoney") t_Money = val;

                    else if (key == "FallbackPerson") t_FallbackPerson = val;
                    else if (key == "FallbackNumber") t_FallbackNumber = val;
                }
                catch (const std::invalid_argument) {
                    logs::warn("VBoS INI Parser: Invalid value for key '{}': {}", key, val);
                }
                catch (const std::out_of_range) {
                    logs::warn("VBoS INI Parser: Value out of range for key '{}': {}", key, val);
                }
            }
        }
        textColor = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    }

    void LoadFont(ImGuiIO& io) {
        static bool fontLoaded = false;
        if (fontLoaded) return;

        if (!fontPath.empty()) {
            std::filesystem::path fontPathObj(reinterpret_cast<const char8_t*>(fontPath.c_str()));
            std::error_code ec;
            if (std::filesystem::exists(fontPathObj, ec)) {
                std::ifstream fontFile(fontPathObj, std::ios::binary | std::ios::ate);
                if (fontFile.is_open()) {
                    std::streamsize size = fontFile.tellg();
                    fontFile.seekg(0, std::ios::beg);

                    void* fontData = IM_ALLOC(size);
                    if (fontFile.read(static_cast<char*>(fontData), size)) {
                        io.Fonts->AddFontFromMemoryTTF(fontData, static_cast<int>(size), fontSize);
                        logs::info("Loaded custom font from memory: {}", fontPath);
                        fontLoaded = true;
                        return;
                    }
                    IM_FREE(fontData);
                }
            }
            logs::warn("Custom font not found: {}. Using default ImGui font.", fontPath);
        }
        ImFontConfig config;
        config.SizePixels = fontSize;
        io.Fonts->AddFontDefault(&config);

        logs::info("Loaded default ImGui font.");
        fontLoaded = true;
    }

    std::string CleanBookText(std::string rawText) {
        if (rawText.empty()) return "";
        std::string cleaned = rawText;

		// Basic cleaning
        static const std::regex r_cr("\r");
        static const std::regex r_pb("\\[pagebreak\\]", std::regex_constants::icase);
        static const std::regex r_br("<br>", std::regex_constants::icase);
        static const std::regex r_img("<img[^>]*Illuminated_Letters/([a-zA-Z])_letter\\.png[^>]*>", std::regex_constants::icase);

		// Change <alias> tags to a more readable format
        static const std::regex r_aPlayer("<Alias=(Player|Friend|Companion|Follower)>", std::regex_constants::icase);
        static const std::regex r_aSpouse("<Alias=(Spouse|Wife|Husband)>", std::regex_constants::icase);
        static const std::regex r_aChild("<Alias=(Child|Son|Daughter)>", std::regex_constants::icase);

        static const std::regex r_aKiller("<Alias=(Killer|Assassin)>", std::regex_constants::icase);
        static const std::regex r_aEnemy("<Alias=(Enemy|Bandit|Vampire|Dragon|Giant)>", std::regex_constants::icase);
        static const std::regex r_aThief("<Alias=(Thief|Kidnapper)>", std::regex_constants::icase);
        static const std::regex r_aBoss("<Alias\\.ShortName=(Boss|Leader)>|<Alias=(Boss|Leader)>", std::regex_constants::icase);

        static const std::regex r_aVictim("<Alias=(Victim)>", std::regex_constants::icase);
        static const std::regex r_aDead("<Alias=(DeadFriend|Deceased)>", std::regex_constants::icase);
        static const std::regex r_aTarget("<Alias=(Target|Mark)>", std::regex_constants::icase);

        static const std::regex r_aJarl("<Alias=(Jarl|Commander)>", std::regex_constants::icase);
        static const std::regex r_aSteward("<Alias=(HomeCitySteward|Steward|Courier)>", std::regex_constants::icase);
        static const std::regex r_aClient("<Alias=(Client|Requester|Questgiver)>", std::regex_constants::icase);

        static const std::regex r_aCity("<Alias=(HomeCity|HoldCity|Capital|City)>", std::regex_constants::icase);
        static const std::regex r_aHold("<Alias=(Hold|Province)>", std::regex_constants::icase);
        static const std::regex r_aLoc("<Alias=(Home|Dungeon|BookLocation|BountyLocation|Location|Destination|Camp|Cave)>", std::regex_constants::icase);

        static const std::regex r_aItem("<Alias=(Item|Weapon|Armor|Artifact|Reward)>", std::regex_constants::icase);
        static const std::regex r_aBook("<Alias=(Book)>", std::regex_constants::icase);

		// Pronouns & race/gender
        static const std::regex r_aRaceGen("<Alias\\.(Race|Gender)=Player>", std::regex_constants::icase);
        static const std::regex r_aPro("<Alias\\.Pronoun=[^>]*>", std::regex_constants::icase);
        static const std::regex r_aProObj("<Alias\\.PronounObj=[^>]*>", std::regex_constants::icase);
        static const std::regex r_aProPosObj("<Alias\\.PronounPosObj=[^>]*>", std::regex_constants::icase);

		// Globals (dates & gold)
        static const std::regex r_gDate("<Global\\.Day[^>]*>[ \t]*<Global\\.MonthWord[^>]*>,[ \t]*<Global\\.Year[^>]*>", std::regex_constants::icase);
        static const std::regex r_gMoney("<Global=WIKill[^>]*>", std::regex_constants::icase);

		// Fallback: remove any remaining tags that weren't specifically handled
        static const std::regex r_aAnyFallback("<Alias[^>]*>[ \t]*,?[ \t]*", std::regex_constants::icase);
        static const std::regex r_gAnyFallback("<Global[^>]*>", std::regex_constants::icase);

        // Layout & formatting
        static const std::regex r_tags("<[^>]*>");
        static const std::regex r_tabs("\t");
        static const std::regex r_spaces_lead("\n[ \t]+");
        static const std::regex r_spaces_trail("[ \t]+\n");
        static const std::regex r_multi_space("[ \t]{2,}");
        static const std::regex r_empty("\n{3,}");

        // Base
        cleaned = std::regex_replace(cleaned, r_cr, "");
        cleaned = std::regex_replace(cleaned, r_pb, "\n");
        cleaned = std::regex_replace(cleaned, r_br, "\n");
        cleaned = std::regex_replace(cleaned, r_img, "$1");

        // Aliases
        cleaned = std::regex_replace(cleaned, r_aPlayer, t_Player);
        cleaned = std::regex_replace(cleaned, r_aSpouse, t_Spouse);
        cleaned = std::regex_replace(cleaned, r_aChild, t_Child);

        cleaned = std::regex_replace(cleaned, r_aKiller, t_Killer);
        cleaned = std::regex_replace(cleaned, r_aEnemy, t_Enemy);
        cleaned = std::regex_replace(cleaned, r_aThief, t_Thief);
        cleaned = std::regex_replace(cleaned, r_aBoss, t_Boss);

        cleaned = std::regex_replace(cleaned, r_aVictim, t_Victim);
        cleaned = std::regex_replace(cleaned, r_aDead, t_Dead);
        cleaned = std::regex_replace(cleaned, r_aTarget, t_Target);

        cleaned = std::regex_replace(cleaned, r_aJarl, t_Jarl);
        cleaned = std::regex_replace(cleaned, r_aSteward, t_Steward);
        cleaned = std::regex_replace(cleaned, r_aClient, t_Client);

        cleaned = std::regex_replace(cleaned, r_aCity, t_City);
        cleaned = std::regex_replace(cleaned, r_aHold, t_Hold);
        cleaned = std::regex_replace(cleaned, r_aLoc, t_Loc);

        cleaned = std::regex_replace(cleaned, r_aItem, t_Item);
        cleaned = std::regex_replace(cleaned, r_aBook, t_Book);

        cleaned = std::regex_replace(cleaned, r_aRaceGen, t_RaceGen);
        cleaned = std::regex_replace(cleaned, r_aPro, t_Pro);
        cleaned = std::regex_replace(cleaned, r_aProObj, t_ProObj);
        cleaned = std::regex_replace(cleaned, r_aProPosObj, t_ProPosObj);

        cleaned = std::regex_replace(cleaned, r_gDate, t_Date);
        cleaned = std::regex_replace(cleaned, r_gMoney, t_Money);

		// Fallback for unhandled tags
        cleaned = std::regex_replace(cleaned, r_aAnyFallback, t_FallbackPerson);
        cleaned = std::regex_replace(cleaned, r_gAnyFallback, t_FallbackNumber);

		// Cleaning the layout and formatting
        cleaned = std::regex_replace(cleaned, r_tags, "");
        cleaned = std::regex_replace(cleaned, r_tabs, " ");
        cleaned = std::regex_replace(cleaned, r_spaces_lead, "\n");
        cleaned = std::regex_replace(cleaned, r_spaces_trail, "\n");
        cleaned = std::regex_replace(cleaned, r_multi_space, " ");

		// Multiple empty lines down to one
        cleaned = std::regex_replace(cleaned, r_empty, "\n\n");

        size_t first = cleaned.find_first_not_of(" \t\n");
        if (first != std::string::npos) cleaned = cleaned.substr(first);
        size_t last = cleaned.find_last_not_of(" \t\n");
        if (last != std::string::npos) cleaned = cleaned.substr(0, last + 1);

        return cleaned;
    }

    void StartSubtitles(RE::TESObjectBOOK* book, int32_t soundID) {
        if (!book) return;

        RE::BSString description;
        book->GetDescription(description, nullptr);

        currentText = CleanBookText(description.c_str());

        if (!currentText.empty()) {
            isReading = true;
            isPaused = false;
            currentScrollY = 0.0f;
            activeSoundID = soundID;
        }
    }

    void StopSubtitles(int32_t soundID) {
        if (soundID != -1 && soundID != activeSoundID) return;

        isReading = false;
        currentText.clear();
		activeSoundID = -1;
    }

    void SetPaused(bool paused) {
        isPaused = paused;
    }

    void Render() {
        if (!isReading || currentText.empty() || !RenderManager::IsReady()) return;

        ImGuiIO& io = ImGui::GetIO();

        if (!isPaused) {
            currentScrollY += (fScrollSpeed * io.DeltaTime);
            if (currentScrollY < 0.0f) {
                currentScrollY = 0.0f;
            }
        }

        auto* ui = RE::UI::GetSingleton();
        bool isMenuOpen = (ui && ui->GameIsPaused());

        if (isPaused || isMenuOpen || !bSubtitlesEnabled) return;

        float screenWidth = io.DisplaySize.x;
        float screenHeight = io.DisplaySize.y;

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration;

        float windowWidth = screenWidth * 0.5f;
        float windowHeight = screenHeight * 0.15f;

        ImGui::SetNextWindowPos(ImVec2((screenWidth - windowWidth) * 0.5f, screenHeight - windowHeight - 80.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);

        if (ImGui::Begin("VBoS_Subtitles", nullptr, flags)) {
            if (ImGui::BeginChild("SubtitlesClip", ImVec2(0, 0), false, flags)) {

                float startY = (ImGui::GetWindowHeight() * 0.5f) - currentScrollY;
                ImGui::SetCursorPosY(startY);

                ImVec2 pos = ImGui::GetCursorScreenPos();
                float wrapWidth = ImGui::GetContentRegionAvail().x;

                ImU32 shadowCol = IM_COL32(0, 0, 0, 255);
                ImU32 textCol = ImGui::ColorConvertFloat4ToU32(textColor);

                auto* drawList = ImGui::GetWindowDrawList();
                auto* font = ImGui::GetFont();
                float currentFontSize = ImGui::GetFontSize();

                drawList->AddText(font, currentFontSize, ImVec2(pos.x + 2.0f, pos.y + 2.0f), shadowCol, currentText.c_str(), nullptr, wrapWidth);
                drawList->AddText(font, currentFontSize, pos, textCol, currentText.c_str(), nullptr, wrapWidth);

                ImVec2 textSize = font->CalcTextSizeA(currentFontSize, FLT_MAX, wrapWidth, currentText.c_str());
                ImGui::Dummy(textSize);

                ImGui::EndChild();
            }
        }
        ImGui::End();
    }

    // Saves the dynamically changed scroll speed back to the INI file
    static void SaveSpeedToINI() {
        std::string iniPath = "Data\\SKSE\\Plugins\\VBoS_Subtitles.ini";
        std::vector<std::string> lines;
        std::ifstream inFile(iniPath);
        bool speedFound = false;

        if (inFile.is_open()) {
            std::string line;
            while (std::getline(inFile, line)) {
                if (line.starts_with("ScrollSpeed")) {
                    lines.push_back(std::format("ScrollSpeed={:.1f}", fScrollSpeed));
                    speedFound = true;
                }
                else {
                    lines.push_back(line);
                }
            }
            inFile.close();
        }

        if (speedFound) {
            std::ofstream outFile(iniPath);
            for (const auto& l : lines) {
                outFile << l << "\n";
            }
        }
    }

    // Captures keyboard input to adjust subtitle scroll speed dynamically
    class InputEventHandler : public RE::BSTEventSink<RE::InputEvent*> {
    private:
        float lastRepeatUp = 0.0f;
        float lastRepeatDown = 0.0f;
        bool pendingSave = false;

        const float INITIAL_DELAY = 0.5f;
        const float REPEAT_RATE = 0.1f;

    public:
        static InputEventHandler* GetSingleton() {
            static InputEventHandler singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override {
            if (!a_event || !*a_event) return RE::BSEventNotifyControl::kContinue;

            for (auto event = *a_event; event; event = event->next) {
                if (event->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
                    auto* button = event->AsButtonEvent();
                    if (button && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {

                        uint32_t key = button->GetIDCode();

                        if (key == keyModifier) {
                            isModifierPressed = button->IsPressed();

                            if (button->IsUp() && pendingSave) {
                                SaveSpeedToINI();
                                pendingSave = false;
                            }
                        }

                        if (key == keySpeedUp) {
                            if (isModifierPressed && button->IsDown()) {
                                fScrollSpeed = std::clamp(fScrollSpeed + 0.5f, -100.0f, 100.0f);
                                pendingSave = true;
                                lastRepeatUp = 0.0f;
                            }
                            else if (isModifierPressed && button->IsPressed()) {
                                float duration = button->HeldDuration();
                                if (duration >= INITIAL_DELAY) {
                                    float activeTime = duration - INITIAL_DELAY;
                                    if (activeTime - lastRepeatUp >= REPEAT_RATE) {
                                        fScrollSpeed = std::clamp(fScrollSpeed + 2.5f, -100.0f, 100.0f);
                                        pendingSave = true;
                                        lastRepeatUp = activeTime;
                                    }
                                }
                            }
                            else if (button->IsUp() && pendingSave) {
                                SaveSpeedToINI();
                                pendingSave = false;
                            }
                        }
                        else if (key == keySpeedDown) {
                            if (isModifierPressed && button->IsDown()) {
                                fScrollSpeed = std::clamp(fScrollSpeed - 0.5f, -100.0f, 100.0f);
                                pendingSave = true;
                                lastRepeatDown = 0.0f;
                            }
                            else if (isModifierPressed && button->IsPressed()) {
                                float duration = button->HeldDuration();
                                if (duration >= INITIAL_DELAY) {
                                    float activeTime = duration - INITIAL_DELAY;
                                    if (activeTime - lastRepeatDown >= REPEAT_RATE) {
                                        fScrollSpeed = std::clamp(fScrollSpeed - 2.5f, -100.0f, 100.0f);
                                        pendingSave = true;
                                        lastRepeatDown = activeTime;
                                    }
                                }
                            }
                            else if (button->IsUp() && pendingSave) {
                                SaveSpeedToINI();
                                pendingSave = false;
                            }
                        }
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void InstallInputSink() {
        auto* deviceManager = RE::BSInputDeviceManager::GetSingleton();
        if (deviceManager) {
            deviceManager->AddEventSink(InputEventHandler::GetSingleton());
            logs::info("Subtitle Input Hook installed.");
        }
    }
}