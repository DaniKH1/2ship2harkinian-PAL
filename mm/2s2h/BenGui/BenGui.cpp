#include "BenGui.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <spdlog/spdlog.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "fast/Fast3dGui.h"
#include <libultraship/window/gui/GfxDebuggerWindow.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "UIWidgets.hpp"
#include "HudEditor.h"
#include "2s2h/Enhancements/Audio/AudioEditor.h"
#include "2s2h/Enhancements/ModMenu/ModMenu.h"
#include "CosmeticEditor.h"
#include "Notification.h"
#include "2s2h/Rando/CheckTracker/CheckTracker.h"

#ifdef __APPLE__
#include <fast/backends/gfx_metal.h>
#endif

#ifdef __SWITCH__
#include <port/switch/SwitchImpl.h>
#endif

#include "include/global.h"
#include "BenPort.h"

#include "Enhancements/Trackers/ItemTracker/ItemTracker.h"
#include "Enhancements/Trackers/ItemTracker/ItemTrackerSettings.h"
#include "Enhancements/Trackers/DisplayOverlay.h"
#include "Enhancements/Trackers//TimeSplits/Timesplits.h"
#include "Enhancements/Trackers/TimeSplits/TimesplitsSettings.h"
#include "BenMenu.h"
#include "BenMenuBar.h"
#include "UiLocalization.h"
#ifdef __ANDROID__
#include <ship/port/mobile/MobileImpl.h>
#endif
#include "DeveloperTools/ActionDebugger.h"
#include "DeveloperTools/HookDebugger.h"
#include "DeveloperTools/SaveEditor.h"
#include "DeveloperTools/ActorViewer.h"
#include "DeveloperTools/CollisionViewer.h"
#include "DeveloperTools/EventLog.h"
#include "DeveloperTools/DLViewer.h"
#include "DeveloperTools/MessageViewer.h"
#include "2s2h/Network/Anchor/Anchor.h"

namespace {
std::atomic_bool gStartupMenuActive{ true };
}

extern "C" bool BenGui_IsStartupMenuActive(void) {
    return gStartupMenuActive.load();
}

namespace BenGui {

void BenMenu::SetVisibility(bool visible) {
    if (visible) {
        CVarSetString("gSettings.Menu.ActiveHeader", "Settings");
        CVarSetString("gSettings.Menu.SettingsSidebarSection", "General");
    }

    Ship::Menu::SetVisibility(visible);

#ifdef __ANDROID__
    // Unlike the toggle-key handler, this also runs for the ImGui close button.
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    if (gui->GetMenuOrMenubarVisible()) {
        Ship::Mobile::DisableTouchArea();
    } else {
        Ship::Mobile::EnableTouchArea();
    }
#endif
}

// MARK: - Delegates

std::shared_ptr<BenMenuBar> mBenMenuBar;

std::shared_ptr<Ship::GuiWindow> mConsoleWindow;
std::shared_ptr<Ship::GuiWindow> mStatsWindow;
std::shared_ptr<Ship::GuiWindow> mGfxDebuggerWindow;
std::shared_ptr<Ship::GuiWindow> mInputEditorWindow;

std::shared_ptr<ActionDebuggerWindow> mActionDebuggerWindow;
std::shared_ptr<HookDebuggerWindow> mHookDebuggerWindow;
std::shared_ptr<SaveEditorWindow> mSaveEditorWindow;
std::shared_ptr<HudEditorWindow> mHudEditorWindow;
std::shared_ptr<CosmeticEditorWindow> mCosmeticEditorWindow;
std::shared_ptr<ActorViewerWindow> mActorViewerWindow;
std::shared_ptr<CollisionViewerWindow> mCollisionViewerWindow;
std::shared_ptr<EventLogWindow> mEventLogWindow;
std::shared_ptr<DLViewerWindow> mDLViewerWindow;
std::shared_ptr<MessageViewerWindow> mMessageViewerWindow;
std::shared_ptr<AudioEditor> mAudioEditorWindow;
std::shared_ptr<ModMenuWindow> mModMenuWindow;
std::shared_ptr<BenMenu> mBenMenu;
std::shared_ptr<Notification::Window> mNotificationWindow;
std::shared_ptr<Rando::CheckTracker::CheckTrackerWindow> mRandoCheckTrackerWindow;
std::shared_ptr<Rando::CheckTracker::SettingsWindow> mRandoCheckTrackerSettingsWindow;
std::shared_ptr<ItemTrackerWindow> mItemTrackerWindow;
std::shared_ptr<ItemTrackerSettingsWindow> mItemTrackerSettingsWindow;
std::shared_ptr<DisplayOverlayWindow> mDisplayOverlayWindow;
std::shared_ptr<TimesplitsWindow> mTimesplitsWindow;
std::shared_ptr<TimesplitsSettingsWindow> mTimesplitsSettingsWindow;
std::shared_ptr<InputViewer> mInputViewer;
std::shared_ptr<InputViewerSettingsWindow> mInputViewerSettings;
std::shared_ptr<BenModalWindow> mModalWindow;
std::shared_ptr<AnchorRoomWindow> mAnchorRoomWindow;

class StartupMenuWindow : public Ship::GuiWindow {
  public:
    StartupMenuWindow()
        : GuiWindow("", false, "2Ship Main Menu", ImVec2(-1, -1),
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse) {
    }

    void InitElement() override {}
    void UpdateElement() override {
        if (gStartupMenuActive.load() && !IsVisible() &&
            (BenGui::mBenMenu == nullptr || !BenGui::mBenMenu->IsVisible()) &&
            (BenGui::mModMenuWindow == nullptr || !BenGui::mModMenuWindow->IsVisible())) {
            Show();
        }
    }

    void Draw() override {
        if (!IsVisible()) {
            return;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        GuiWindow::Draw();
        ImGui::PopStyleVar(2);
    }

    void DrawElement() override {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 origin = viewport->Pos;
        const ImVec2 size = viewport->Size;
        const ImVec2 bottomRight(origin.x + size.x, origin.y + size.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, bottomRight, IM_COL32(40, 29, 35, 255));

        ImFont* logoFont = OTRGlobals::Instance->fontTriforce;

        auto gui = std::dynamic_pointer_cast<Fast::Fast3dGui>(
            Ship::Context::GetRawInstance()->GetWindow()->GetGui());
        if (gui != nullptr) {
            if (!gui->HasTextureByName("Startup-Majora-Mask")) {
                const HRSRC resource = FindResourceW(nullptr, L"STARTUP_MASK", RT_RCDATA);
                const HGLOBAL resourceHandle = resource != nullptr ? LoadResource(nullptr, resource) : nullptr;
                const DWORD resourceSize = resource != nullptr ? SizeofResource(nullptr, resource) : 0;
                const void* resourceData = resourceHandle != nullptr ? LockResource(resourceHandle) : nullptr;
                if (resourceData != nullptr && resourceSize > 0) {
                    gui->LoadTextureFromEncodedImage("Startup-Majora-Mask",
                                                     static_cast<const uint8_t*>(resourceData), resourceSize);
                } else {
                    SPDLOG_WARN("Could not load embedded startup background (Windows error {}).", GetLastError());
                }
            }

            const ImTextureID maskTexture = gui->GetTextureByName("Startup-Majora-Mask");
            if (maskTexture != nullptr) {
                const ImVec2 cropMin(0.08f, 0.25f);
                const ImVec2 cropMax(0.92f, 0.72f);
                constexpr float cropAspect = (0.84f * 1125.0f) / (0.47f * 2000.0f);
                const float artWidth = std::min(size.x * 0.56f, size.y * 0.92f * cropAspect);
                const float artHeight = artWidth / cropAspect;
                const ImVec2 artMin(origin.x + size.x * 0.48f, origin.y + (size.y - artHeight) * 0.5f);
                ImGui::SetCursorScreenPos(artMin);
                ImGui::Image(const_cast<void*>(maskTexture), ImVec2(artWidth, artHeight), cropMin, cropMax);
            }
        }

        const float scale = std::clamp(size.x / 1280.0f, 0.8f, 1.8f);
        const float left = origin.x + size.x * 0.072f;
        const char* logo = "2Ship2Harkinian";
        float logoSize = std::min(size.y * 0.20f, size.x * 0.075f);
        const ImVec2 logoPos(left, origin.y + size.y * 0.17f);
        if (logoFont != nullptr) {
            const ImVec2 logoTextSize = logoFont->CalcTextSizeA(logoSize, FLT_MAX, 0.0f, logo);
            if (logoTextSize.x > size.x * 0.43f) {
                logoSize *= size.x * 0.43f / logoTextSize.x;
            }
            drawList->AddText(logoFont, logoSize, ImVec2(logoPos.x + 3.0f, logoPos.y + 4.0f),
                              IM_COL32(25, 16, 33, 255), logo);
            drawList->AddText(logoFont, logoSize, ImVec2(logoPos.x - 1.0f, logoPos.y - 1.0f),
                              IM_COL32(209, 137, 229, 255), logo);
            drawList->AddText(logoFont, logoSize, logoPos, IM_COL32(139, 66, 177, 255), logo);
        }

        const char* menuLabels[] = { "Start Game", "Settings", "Mods", "Exit" };
        const float buttonTop = origin.y + size.y * 0.53f;
        const ImVec2 buttonSize(size.x * 0.32f, 48.0f * scale);
        static int selectedItem = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
            selectedItem = (selectedItem + 3) % 4;
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
            selectedItem = (selectedItem + 1) % 4;
        }

        for (int i = 0; i < 4; ++i) {
            const ImVec2 buttonPos(left, buttonTop + i * buttonSize.y);
            ImGui::SetCursorScreenPos(buttonPos);
            const std::string itemId = std::string("##startup-menu-") + std::to_string(i);
            const bool clicked = ImGui::InvisibleButton(itemId.c_str(), buttonSize);
            const bool hovered = ImGui::IsItemHovered();
            if (hovered) {
                selectedItem = i;
            }
            const bool selected = selectedItem == i;
            if (selected) {
                drawList->AddRectFilled(buttonPos, ImVec2(buttonPos.x + buttonSize.x, buttonPos.y + buttonSize.y),
                                        IM_COL32(70, 56, 49, 175));
                drawList->AddRectFilled(buttonPos, ImVec2(buttonPos.x + 4.0f * scale, buttonPos.y + buttonSize.y),
                                        IM_COL32(255, 190, 83, 255));
            }
            const char* label = BenGui::TranslateUiText(menuLabels[i]);
            const ImU32 labelColor = selected ? IM_COL32(255, 196, 91, 255) : IM_COL32(242, 240, 244, 255);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.65f,
                              ImVec2(buttonPos.x + 18.0f * scale, buttonPos.y + 8.0f * scale), labelColor, label);

            if (clicked || (selected && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
                switch (i) {
                    case 0: {
                        std::string modStartError;
                        if (!ModMenu_StartGame(modStartError)) {
                            BenGui::RegisterPopup("Mod Loader Error", modStartError);
                            break;
                        }
                        if (BenGui::mBenMenu != nullptr) {
                            BenGui::mBenMenu->Hide();
                        }
                        if (BenGui::mModMenuWindow != nullptr) {
                            BenGui::mModMenuWindow->Hide();
                        }
                        gStartupMenuActive.store(false);
                        Hide();
                        break;
                    }
                    case 1:
                        Hide();
                        if (BenGui::mModMenuWindow != nullptr) {
                            BenGui::mModMenuWindow->Hide();
                        }
                        if (BenGui::mBenMenu != nullptr) {
                            BenGui::mBenMenu->Show();
                        }
                        break;
                    case 2:
                        Hide();
                        if (BenGui::mBenMenu != nullptr) {
                            BenGui::mBenMenu->Hide();
                        }
                        if (BenGui::mModMenuWindow != nullptr) {
                            BenGui::mModMenuWindow->Show();
                        }
                        break;
                    case 3:
                        Ship::Context::GetRawInstance()->GetWindow()->Close();
                        break;
                }
            }
        }

    }
};

static std::shared_ptr<StartupMenuWindow> mStartupMenuWindow;

UIWidgets::Colors GetMenuThemeColor() {
    return UIWidgets::GetMenuThemeColor();
}

void SetupMenu() {
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    CVarSetInteger("gWindows.Menu", 0);
    Ship::Context::GetRawInstance()->GetConsoleVariables()->Save();
    mBenMenu = std::make_shared<BenMenu>("gWindows.Menu", "Settings Menu");
    gui->SetMenu(mBenMenu);

    auto& style = ImGui::GetStyle();
#if defined(__ANDROID__)
    // libultraship already called ScaleAllSizes() at startup; don't compound
    // that global scale when applying the Android-specific touch style.
    const float androidMenuScale = CVarGetFloat("gSettings.Menu.AndroidScale", AndroidMenuStyle::DefaultScale);
    AndroidMenuStyle::Apply(androidMenuScale);
#else
    style.FramePadding = ImVec2(4.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
#endif
    style.Colors[ImGuiCol_MenuBarBg] = UIWidgets::ColorValues.at(UIWidgets::Colors::DarkGray);

    mModalWindow = std::make_shared<BenModalWindow>("gWindows.ModalWindow", "Modal Window");
    gui->AddGuiWindow(mModalWindow);
    mModalWindow->Show();
}

void SetupGuiElements() {
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    gStartupMenuActive.store(true);

    mStatsWindow = gui->GetGuiWindow("Stats");
    if (mStatsWindow == nullptr) {
        SPDLOG_ERROR("Could not find stats window");
    }

    mConsoleWindow = gui->GetGuiWindow("Console");
    if (mConsoleWindow == nullptr) {
        SPDLOG_ERROR("Could not find console window");
    }

    mGfxDebuggerWindow =
        std::make_shared<LUS::GfxDebuggerWindow>("gOpenWindows.GfxDebugger", "Gfx Debugger", ImVec2(520, 600));
    gui->AddGuiWindow(mGfxDebuggerWindow);

    mInputEditorWindow = gui->GetGuiWindow("2S2H Input Editor");
    if (mInputEditorWindow == nullptr) {
        SPDLOG_ERROR("Could not find input editor window");
    }

    mActionDebuggerWindow =
        std::make_shared<ActionDebuggerWindow>("gWindows.ActionDebugger", "Action Debugger", ImVec2(560, 680));
    gui->AddGuiWindow(mActionDebuggerWindow);

    mHookDebuggerWindow =
        std::make_shared<HookDebuggerWindow>("gWindows.HookDebugger", "Hook Debugger", ImVec2(480, 600));
    gui->AddGuiWindow(mHookDebuggerWindow);

    mSaveEditorWindow = std::make_shared<SaveEditorWindow>("gWindows.SaveEditor", "Save Editor", ImVec2(480, 600));
    gui->AddGuiWindow(mSaveEditorWindow);

    mHudEditorWindow = std::make_shared<HudEditorWindow>("gWindows.HudEditor", "HUD Editor", ImVec2(480, 600));
    gui->AddGuiWindow(mHudEditorWindow);

    mCosmeticEditorWindow =
        std::make_shared<CosmeticEditorWindow>("gWindows.CosmeticEditor", "Cosmetic Editor", ImVec2(480, 600));
    gui->AddGuiWindow(mCosmeticEditorWindow);

    mActorViewerWindow = std::make_shared<ActorViewerWindow>("gWindows.ActorViewer", "Actor Viewer", ImVec2(520, 600));
    gui->AddGuiWindow(mActorViewerWindow);

    mCollisionViewerWindow =
        std::make_shared<CollisionViewerWindow>("gWindows.CollisionViewer", "Collision Viewer", ImVec2(390, 475));
    gui->AddGuiWindow(mCollisionViewerWindow);

    mEventLogWindow = std::make_shared<EventLogWindow>("gWindows.EventLog", "Event Log", ImVec2(520, 600));
    gui->AddGuiWindow(mEventLogWindow);

    mDLViewerWindow = std::make_shared<DLViewerWindow>("gWindows.DLViewer", "DL Viewer", ImVec2(520, 600));
    gui->AddGuiWindow(mDLViewerWindow);
    mMessageViewerWindow =
        std::make_shared<MessageViewerWindow>("gWindows.MessageViewer", "Message Viewer", ImVec2(520, 600));
    gui->AddGuiWindow(mMessageViewerWindow);

    mAudioEditorWindow = std::make_shared<AudioEditor>("gWindows.AudioEditor", "Audio Editor", ImVec2(520, 600));
    gui->AddGuiWindow(mAudioEditorWindow);

    mModMenuWindow = std::make_shared<ModMenuWindow>(
        "gWindows.ModMenu", false, "Mod Menu", ImVec2(-1, -1),
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    gui->AddGuiWindow(mModMenuWindow);
    mModMenuWindow->Hide();

    mItemTrackerWindow = std::make_shared<ItemTrackerWindow>("gWindows.ItemTracker", "Item Tracker");
    gui->AddGuiWindow(mItemTrackerWindow);

    mItemTrackerSettingsWindow = std::make_shared<ItemTrackerSettingsWindow>("gWindows.ItemTrackerSettings",
                                                                             "Item Tracker Settings", ImVec2(800, 400));
    gui->AddGuiWindow(mItemTrackerSettingsWindow);

    mDisplayOverlayWindow = std::make_shared<DisplayOverlayWindow>("gWindows.DisplayOverlay", "Display Overlay");
    gui->AddGuiWindow(mDisplayOverlayWindow);

    mTimesplitsWindow = std::make_shared<TimesplitsWindow>("gWindows.Timesplits", "Time Splits Window");
    gui->AddGuiWindow(mTimesplitsWindow);

    mTimesplitsSettingsWindow = std::make_shared<TimesplitsSettingsWindow>(
        "gWindows.Timesplits.Settings", "Time Splits Settings Window", ImVec2(567, 97));
    gui->AddGuiWindow(mTimesplitsSettingsWindow);

    mNotificationWindow = std::make_shared<Notification::Window>("gWindows.Notifications", "Notifications Window");
    gui->AddGuiWindow(mNotificationWindow);
    mNotificationWindow->Show();

    mRandoCheckTrackerWindow = std::make_shared<Rando::CheckTracker::CheckTrackerWindow>(
        "gWindows.CheckTracker", "Check Tracker", ImVec2(375, 460));
    gui->AddGuiWindow(mRandoCheckTrackerWindow);

    mRandoCheckTrackerSettingsWindow = std::make_shared<Rando::CheckTracker::SettingsWindow>(
        "gWindows.CheckTrackerSettings", "Check Tracker Settings");
    gui->AddGuiWindow(mRandoCheckTrackerSettingsWindow);

    mInputViewer = std::make_shared<InputViewer>("gWindows.InputViewer", "Input Viewer");
    gui->AddGuiWindow(mInputViewer);
    mInputViewerSettings = std::make_shared<InputViewerSettingsWindow>("gWindows.InputViewerSettings",
                                                                       "Input Viewer Settings", ImVec2(500, 525));
    gui->AddGuiWindow(mInputViewerSettings);

    mAnchorRoomWindow = std::make_shared<AnchorRoomWindow>("gWindows.AnchorRoom", "Anchor Room");
    gui->AddGuiWindow(mAnchorRoomWindow);

    mStartupMenuWindow = std::make_shared<StartupMenuWindow>();
    gui->AddGuiWindow(mStartupMenuWindow);
    mStartupMenuWindow->Show();
}

void Destroy() {
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();

    gui->RemoveAllGuiWindows();
    mBenMenuBar = nullptr;
    mBenMenu = nullptr;
    mModalWindow = nullptr;
    mStatsWindow = nullptr;
    mConsoleWindow = nullptr;
    mGfxDebuggerWindow = nullptr;
    mInputEditorWindow = nullptr;
    mCollisionViewerWindow = nullptr;
    mEventLogWindow = nullptr;
    mNotificationWindow = nullptr;
    mRandoCheckTrackerWindow = nullptr;
    mRandoCheckTrackerSettingsWindow = nullptr;

    mActionDebuggerWindow = nullptr;
    mHookDebuggerWindow = nullptr;
    mSaveEditorWindow = nullptr;
    mHudEditorWindow = nullptr;
    mCosmeticEditorWindow = nullptr;
    mActorViewerWindow = nullptr;
    mDLViewerWindow = nullptr;
    mMessageViewerWindow = nullptr;
    mAudioEditorWindow = nullptr;
    mModMenuWindow = nullptr;
    mItemTrackerWindow = nullptr;
    mItemTrackerSettingsWindow = nullptr;
    mInputViewer = nullptr;
    mInputViewerSettings = nullptr;
    mAnchorRoomWindow = nullptr;
    mStartupMenuWindow = nullptr;
}

void RegisterPopup(std::string title, std::string message, std::string button1, std::string button2,
                   std::function<void()> button1callback, std::function<void()> button2callback) {
    mModalWindow->RegisterPopup(title, message, button1, button2, button1callback, button2callback);
}

size_t PopupsQueued() {
    return mModalWindow->PopupsQueued();
}

bool DismissPopup(std::string title) {
    if (mModalWindow->IsPopupOpen(title)) {
        mModalWindow->DismissPopup();
        return true;
    }
    return false;
}

void SetDisplayOverlayVisibility(bool visible) {
    if (mDisplayOverlayWindow != nullptr) {
        if (visible) {
            mDisplayOverlayWindow->Show();
        } else {
            mDisplayOverlayWindow->Hide();
        }
    } else {
        CVarSetInteger("gWindows.DisplayOverlay", visible ? 1 : 0);
    }
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

} // namespace BenGui
