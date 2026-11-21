#include "Anchor.h"
#include <libultraship/libultraship.h>
#include "2s2h/BenGui/UIWidgets.hpp"
#include "ShipUtils.h"
#include "2s2h/BenGui/BenMenu.h"
#include "2s2h/BenGui/BenGui.hpp"
#include "2s2h/BenGui/Notification.h"
#include "2s2h/BenGui/UiLocalization.h"
#include "2s2h/Enhancements/ModMenu/ModMenu.h"
#include "2s2h/Network/Sail/Sail.h"
#include "2s2h/Rando/Spoiler/Spoiler.h"
#include "2s2h/Rando/MiscBehavior/MiscBehavior.h"

namespace BenGui {
extern std::shared_ptr<BenMenu> mBenMenu;
extern std::shared_ptr<AnchorRoomWindow> mAnchorRoomWindow;
} // namespace BenGui
using namespace UIWidgets;

static const char* pvpModes[3] = { "Off", "On", "On + Friendly Fire" };
static std::vector<const char*> teleportModes = { "None", "Team Only", "All" };
static std::vector<const char*> showLocationsModes = { "None", "Team Only", "All" };

void AnchorMainMenu(WidgetInfo& info) {
    auto anchor = Anchor::Instance;

    std::string host = CVarGetString("gNetwork.Anchor.Host", "anchor.hm64.org");
    uint16_t port = CVarGetInteger("gNetwork.Anchor.Port", 43383);
    std::string anchorTeamId = CVarGetString("gNetwork.Anchor.TeamId", "default");
    std::string anchorRoomId = CVarGetString("gNetwork.Anchor.RoomId", "");
    std::string anchorName = CVarGetString("gNetwork.Anchor.Name", "");
    bool isFormValid = !IsStringEmpty(host) && port > 1024 && port < 65535 && !IsStringEmpty(anchorRoomId) &&
                       !IsStringEmpty(anchorName);

    const ImVec4 accent = UIWidgets::ColorValues.at(THEME_COLOR);

    // Hero bar, styled like the new Mod Menu
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(27, 31, 40, 250));
    ImGui::BeginChild("anchor-hero", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(accent, "Anchor Multiplayer");
    ImGui::TextWrapped("Play together in the same room - share items and flags");
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Spacing();

    // Connection Settings card (moved to the top of the menu)
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(34, 37, 45, 230));
    ImGui::BeginChild("anchor-config-card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::SeparatorText("Connection Settings");

    ImGui::BeginDisabled(anchor->isEnabled);
    ImGui::Text("Host & Port");
    if (UIWidgets::InputString("##Host", &host,
                               UIWidgets::InputOptions()
                                   .Size(ImGui::GetContentRegionAvail() -
                                         ImVec2((ImGui::GetFontSize() * 5 + ImGui::GetStyle().ItemSpacing.x), 0))
                                   .Color(THEME_COLOR))) {
        CVarSetString("gNetwork.Anchor.Host", host.c_str());
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    ImGui::SameLine();
    UIWidgets::PushStyleInput(THEME_COLOR);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    if (ImGui::InputScalar("##Port", ImGuiDataType_U16, &port)) {
        CVarSetInteger("gNetwork.Anchor.Port", port);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    UIWidgets::PopStyleInput();

    ImGui::Text("Name");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (UIWidgets::InputString("##Name", &anchorName, UIWidgets::InputOptions().Color(THEME_COLOR))) {
        CVarSetString("gNetwork.Anchor.Name", anchorName.c_str());
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::Text("Room ID");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (UIWidgets::InputString("##RoomId", &anchorRoomId,
                               UIWidgets::InputOptions().IsSecret(anchor->isEnabled).Color(THEME_COLOR))) {
        CVarSetString("gNetwork.Anchor.RoomId", anchorRoomId.c_str());
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::Text("Team ID (Items Shared)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (UIWidgets::InputString("##TeamId", &anchorTeamId, UIWidgets::InputOptions().Color(THEME_COLOR))) {
        CVarSetString("gNetwork.Anchor.TeamId", anchorTeamId.c_str());
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Set a Team ID to share items with your friends. Leave it empty otherwise.");
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Spacing();

}

void AnchorMainMenuRight(WidgetInfo& info) {
    auto anchor = Anchor::Instance;
    std::string host = CVarGetString("gNetwork.Anchor.Host", "anchor.hm64.org");
    uint16_t port = CVarGetInteger("gNetwork.Anchor.Port", 43383);
    std::string anchorRoomId = CVarGetString("gNetwork.Anchor.RoomId", "");
    std::string anchorName = CVarGetString("gNetwork.Anchor.Name", "");
    bool isFormValid = !IsStringEmpty(host) && port > 1024 && port < 65535 && !IsStringEmpty(anchorRoomId) &&
                       !IsStringEmpty(anchorName);

    // Current Room card
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(36, 47, 57, 245));
    ImGui::BeginChild("anchor-room-card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::SeparatorText("Current Room");
    if (!anchor->isEnabled) {
        ImGui::TextDisabled("Anchor is disabled");
    } else if (!anchor->isConnected) {
        ImGui::TextDisabled("Connecting...");
    } else {
        ImGui::Text("%s Connected", ICON_FA_CHECK);
        ImGui::Spacing();
        if (UIWidgets::Button("Request Team State", UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
            anchor->SendPacket_RequestTeamState();
        }
        UIWidgets::Tooltip("Try this if you are missing items that your team members have collected");
        ImGui::SameLine();
        UIWidgets::WindowButton("Toggle Anchor Room Window", "gWindows.AnchorRoom", BenGui::mAnchorRoomWindow);
        if (!BenGui::mAnchorRoomWindow->IsVisible()) {
            BenGui::mAnchorRoomWindow->DrawElement();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Spacing();

    // Enable / Disable + quick actions card
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(34, 37, 45, 230));
    ImGui::BeginChild("anchor-actions-card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::BeginDisabled(!isFormValid);
    const char* buttonLabel = anchor->isEnabled ? "Disable" : "Enable";
    UIWidgets::PushStyleButton(anchor->isEnabled ? UIWidgets::ColorValues.at(UIWidgets::Colors::Red)
                                                 : UIWidgets::ColorValues.at(UIWidgets::Colors::Green));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Button(buttonLabel, ImVec2(-1.0f, 0.0f))) {
        if (anchor->isEnabled) {
            CVarClear("gNetwork.Anchor.Enabled");
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            anchor->Disable();
        } else {
            CVarSetInteger("gNetwork.Anchor.Enabled", 1);
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            anchor->Enable();
        }
    }
    UIWidgets::PopStyleButton();
    ImGui::EndDisabled();
    if (isFormValid) {
        ImGui::TextDisabled("Fill in a Host, Room ID and Name, then Enable Anchor to connect");
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    UIWidgets::PushStyleButton(THEME_COLOR);
    ImGui::SetNextItemWidth(-1.0f);
    if (UIWidgets::Button("Restore Defaults")) {
        CVarSetString("gNetwork.Anchor.Host", "anchor.hm64.org");
        CVarSetInteger("gNetwork.Anchor.Port", 43383);
        CVarSetString("gNetwork.Anchor.TeamId", "default");
        CVarSetString("gNetwork.Anchor.RoomId", "");
        CVarSetString("gNetwork.Anchor.Name", "");
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (UIWidgets::Button("Global Room", UIWidgets::ButtonOptions()
                                             .Color(UIWidgets::Colors::Blue)
                                             .Tooltip("Always-online public room so you don't have to experience "
                                                      "Termina alone. PVP and syncing are disabled."))) {
        CVarSetString("gNetwork.Anchor.Host", "anchor.hm64.org");
        CVarSetInteger("gNetwork.Anchor.Port", 43383);
        CVarSetString("gNetwork.Anchor.TeamId", "default");
        CVarSetString("gNetwork.Anchor.RoomId", "2ship-global");
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    UIWidgets::PopStyleButton();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void AnchorAdminMenu(WidgetInfo& info) {
    auto anchor = Anchor::Instance;
    bool isGlobalRoom = (std::string("2ship-global") == CVarGetString("gNetwork.Anchor.RoomId", ""));

    if (!anchor->isEnabled || !anchor->isConnected || anchor->roomState.ownerClientId != anchor->ownClientId ||
        isGlobalRoom) {
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(34, 37, 45, 230));
    ImGui::BeginChild("anchor-admin-card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::SeparatorText("Room Settings (Admin Only)");

    UIWidgets::PushStyleButton(THEME_COLOR);
    if (ImGui::Button("Clear All Team State")) {
        std::set<std::string> teams;
        for (auto& [clientId, client] : Anchor::Instance->clients) {
            teams.insert(client.teamId);
        }
        for (auto& team : teams) {
            anchor->SendPacket_ClearTeamState(team);
        }
    }
    UIWidgets::PopStyleButton();

    if (UIWidgets::CVarCombobox("PvP Mode:", "gNetwork.Anchor.RoomSettings.PvpMode", pvpModes,
                                UIWidgets::ComboboxOptions()
                                    .DefaultIndex(2)
                                    .LabelPosition(UIWidgets::LabelPosition::Above)
                                    .Color(THEME_COLOR))) {
        anchor->SendPacket_UpdateRoomState();
    }
    if (UIWidgets::CVarCombobox("Show Locations For:", "gNetwork.Anchor.RoomSettings.ShowLocationsMode",
                                showLocationsModes,
                                UIWidgets::ComboboxOptions()
                                    .DefaultIndex(1)
                                    .LabelPosition(UIWidgets::LabelPosition::Above)
                                    .Color(THEME_COLOR))) {
        anchor->SendPacket_UpdateRoomState();
    }
    if (UIWidgets::CVarCombobox("Allow Teleporting To:", "gNetwork.Anchor.RoomSettings.TeleportMode", teleportModes,
                                UIWidgets::ComboboxOptions()
                                    .DefaultIndex(1)
                                    .LabelPosition(UIWidgets::LabelPosition::Above)
                                    .Color(THEME_COLOR))) {
        anchor->SendPacket_UpdateRoomState();
    }
    if (UIWidgets::CVarCheckbox("Sync Items & Flags", "gNetwork.Anchor.RoomSettings.SyncItemsAndFlags",
                                UIWidgets::CheckboxOptions().DefaultValue(true).Color(THEME_COLOR))) {
        anchor->SendPacket_UpdateRoomState();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void AnchorHowToUseMenu(WidgetInfo& info) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(34, 37, 45, 230));
    ImGui::BeginChild("anchor-how-to-use-card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextWrapped(
        "Important Notes:\n- All players involved should start at the file select screen with Anchor disabled.\n- "
        "Archipelago and Anchor can be used together, but make sure you disable item syncing in Anchor!");

    ImGui::TextWrapped("1. The host player should set up a room by entering a Room ID and enable Anchor. (If just one "
                       "team, leave the team as 'default')");

    ImGui::TextWrapped("2. Configure the room settings as desired (PvP, teleporting, etc).");

    ImGui::TextWrapped("3. If multiple teams, other team leaders should enter the same Room ID and their associated "
                       "Team IDs and enable Anchor.");

    ImGui::TextWrapped("4. Host/Team leaders should then create a randomizer seed like normal, and load into the game. "
                       "This can match across teams but doesn't have to.");

    ImGui::TextWrapped("4. Once the Host/Team leaders are in-game, other players can enter the same Room ID and "
                       "associated Team IDs and enable Anchor.");

    ImGui::TextWrapped("5. Other players should then each create a new randomizer file (the configuration does not "
                       "matter, it will be overridden by the team leader's settings), and load into the game.");

    ImGui::TextWrapped("6. All players should now be connected and able to see each other in-game, occasionally you "
                       "may need to leave and re-enter South clock town if a player does not appear.");
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void SailHeroWidget(WidgetInfo& info) {
    const ImVec4 accent = UIWidgets::ColorValues.at(THEME_COLOR);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(27, 31, 40, 250));
    ImGui::BeginChild("sail-hero", ImVec2(0.0f, 82.0f), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(accent, "Sail");
    ImGui::TextDisabled(BenGui::TranslateUiText("A networking protocol for remote control of the client"));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Spacing();
}

void SailCardWidget(WidgetInfo& info) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(34, 37, 45, 230));
    ImGui::BeginChild("sail-card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::SeparatorText(BenGui::TranslateUiText("Connection"));
    ImGui::Spacing();

    ImGui::BeginDisabled(Sail::Instance->isEnabled);
    ImGui::Text(BenGui::TranslateUiText("Host & Port"));
    CVarInputString("##HostSail", "gNetwork.Sail.Host",
                    InputOptions()
                        .PlaceholderText("127.0.0.1")
                        .DefaultValue("127.0.0.1")
                        .Size(ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 7, 0))
                        .LabelPosition(LabelPosition::None));
    ImGui::SameLine();
    ImGui::Text(":");
    ImGui::SameLine();
    CVarInputInt("##PortSail", "gNetwork.Sail.Port",
                 InputOptions()
                     .PlaceholderText("43384")
                     .DefaultValue("43384")
                     .Size(ImVec2(ImGui::GetFontSize() * 5, 0))
                     .LabelPosition(LabelPosition::None));
    ImGui::EndDisabled();
    ImGui::Spacing();

    std::string host = CVarGetString("gNetwork.Sail.Host", "127.0.0.1");
    uint16_t port = CVarGetInteger("gNetwork.Sail.Port", 43384);
    ImGui::BeginDisabled(!(!IsStringEmpty(host) && port > 1024 && port < 65535));
    UIWidgets::PushStyleButton(THEME_COLOR);
    const bool isEnabled = Sail::Instance->isEnabled;
    ImGui::SetNextItemWidth(-1.0f);
    if (UIWidgets::Button(isEnabled ? BenGui::TranslateUiText("Disable") : BenGui::TranslateUiText("Enable"),
                          UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
        if (isEnabled) {
            CVarClear("gNetwork.Sail.Enabled");
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            Sail::Instance->Disable();
        } else if (Sail::Instance->Enable()) {
            CVarSetInteger("gNetwork.Sail.Enabled", 1);
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        } else {
            Notification::Emit({ .message = BenGui::TranslateUiText("Couldn't resolve the Sail host") });
        }
    }
    UIWidgets::PopStyleButton();
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextDisabled(isEnabled ? (Sail::Instance->isConnected ? BenGui::TranslateUiText("Connected")
                                                                 : BenGui::TranslateUiText("Connecting..."))
                                  : BenGui::TranslateUiText("Sail is disabled - set a valid host & port to enable"));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped(BenGui::TranslateUiText("Sail is a networking protocol designed to facilitate remote control of the "
                                              "2Ship2Harkinian client. It is intended to be utilized alongside a Sail server, "
                                              "for which we provide a few straightforward implementations on our GitHub. The "
                                              "current implementations available allow integration with Twitch chat and SAMMI "
                                              "Bot, feel free to contribute your own!"));
    UIWidgets::PushStyleButton(THEME_COLOR);
    ImGui::SetNextItemWidth(-1.0f);
    const std::string linkLabel =
        std::string(ICON_FA_CLIPBOARD) + " " + BenGui::TranslateUiText("Copy GitHub link");
    if (UIWidgets::Button(linkLabel.c_str(),
                          UIWidgets::ButtonOptions().Tooltip("https://github.com/HarbourMasters/sail"))) {
        ImGui::SetClipboardText("https://github.com/HarbourMasters/sail");
        Notification::Emit({ .message = BenGui::TranslateUiText("Copied to clipboard") });
    }
    UIWidgets::PopStyleButton();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void AnchorSfxSettingsMenu(WidgetInfo& info) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(34, 37, 45, 230));
    ImGui::BeginChild("anchor-sfx-settings-card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::SeparatorText("Remote Player SFX");
    ImGui::Spacing();
    UIWidgets::CVarSliderFloat("Kamaro & Bremen SFX Distance", "gNetwork.Anchor.SfxDistanceMultiplier",
                               UIWidgets::FloatSliderOptions()
                                   .Min(0.0f)
                                   .Max(2.0f)
                                   .DefaultValue(1.0f)
                                   .Color(THEME_COLOR)
                                   .Tooltip("Modifies how far the SFX of the Kamaro and Bremen masks can be heard when "
                                            "used by the remote player. 1 is the default distance."));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void RegisterAnchorMenu() {
    // Add Network Menu
    BenGui::mBenMenu->AddMenuEntry("Network", "gSettings.Menu.NetworkSidebarSection");

    BenGui::mBenMenu->AddSidebarEntry("Network", "Anchor", 2);
    WidgetPath path = { "Network", "Anchor", SECTION_COLUMN_1 };
    BenGui::mBenMenu->AddWidget(path, "AnchorMainMenu", WIDGET_CUSTOM).CustomFunction(AnchorMainMenu);
    path.column = SECTION_COLUMN_2;
    BenGui::mBenMenu->AddWidget(path, "AnchorAdminMenu", WIDGET_CUSTOM).CustomFunction(AnchorAdminMenu);
    BenGui::mBenMenu->AddWidget(path, "AnchorMainMenuRight", WIDGET_CUSTOM).CustomFunction(AnchorMainMenuRight);

    // Sail, below the Anchor menu
    BenGui::mBenMenu->AddSidebarEntry("Network", "Sail", 3);
    WidgetPath sailPath = { "Network", "Sail", SECTION_COLUMN_1 };
    BenGui::mBenMenu->AddWidget(sailPath, "Sail Hero", WIDGET_CUSTOM).CustomFunction(SailHeroWidget);
    BenGui::mBenMenu->AddWidget(sailPath, "Sail Card", WIDGET_CUSTOM).CustomFunction(SailCardWidget);

    // Skin Selector category, below the Anchor menu
    BenGui::mBenMenu->AddSidebarEntry("Network", "Skin Selector", 1);
    path = { "Network", "Skin Selector", SECTION_COLUMN_1 };
    BenGui::mBenMenu->AddWidget(path, "Skin Selector", WIDGET_SEPARATOR_TEXT);
    BenGui::mBenMenu->AddWidget(path, "Skin Selector List", WIDGET_CUSTOM)
        .CustomFunction([](WidgetInfo&) { DrawSkinSelector(); });

    // Settings category, below the Skin Selector menu
    BenGui::mBenMenu->AddSidebarEntry("Network", "Settings", 1);
    path = { "Network", "Settings", SECTION_COLUMN_1 };
    BenGui::mBenMenu->AddWidget(path, "AnchorSfxSettingsMenu", WIDGET_CUSTOM).CustomFunction(AnchorSfxSettingsMenu);

    // How to Use category, at the bottom
    BenGui::mBenMenu->AddSidebarEntry("Network", "How to Use", 1);
    path = { "Network", "How to Use", SECTION_COLUMN_1 };
    BenGui::mBenMenu->AddWidget(path, "AnchorHowToUseMenu", WIDGET_CUSTOM).CustomFunction(AnchorHowToUseMenu);
}

static RegisterMenuInitFunc menuInitFunc(RegisterAnchorMenu);
