#include <map>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <libultraship/classes.h>
#include <ship/utils/StringHelper.h>

#include "ModMenu.h"
#include "ModCatalog.h"
#include "2s2h/BenPort.h"
#include "2s2h/resource/type/Skeleton.h"
#include "2s2h/BenGui/MenuTypes.h"
#include "2s2h/BenGui/BenMenu.h"
#include "2s2h/BenGui/BenGui.hpp"
#include "2s2h/BenGui/UiLocalization.h"

extern "C" bool BenGui_IsStartupMenuActive(void);

std::vector<std::string> enabledModFiles;
std::vector<std::string> disabledModFiles;
std::vector<std::string> unsupportedFiles;
std::map<std::string, std::filesystem::path> filePaths;
static std::filesystem::path modsDirectory;
static size_t dragSourceIndex = SIZE_MAX;
static size_t dragTargetIndex = SIZE_MAX;
static char modSearchQuery[256] = {};
static std::map<std::string, std::string> selectedVariantFolders;
static void ApplyVariantDefaults(bool& changed);

struct ModGroup {
    std::string key;
    std::string name;
    std::vector<std::string> files;
    struct OptionalGroup {
        std::string name;
        std::vector<std::string> files;
    };
    std::vector<OptionalGroup> optionalGroups;
    struct VariantOption {
        std::string name;
        std::vector<std::string> files;
    };
    struct VariantGroup {
        std::string name;
        std::vector<VariantOption> options;
    };
    std::vector<VariantGroup> variantGroups;
};

namespace BenGui {
extern std::shared_ptr<BenMenu> mBenMenu;
}

static WidgetInfo enableModsWidget;
static WidgetInfo tabHotkeyWidget;

#define CVAR_ENABLED_MODS_NAME "gSettings.EnabledMods"
#define CVAR_ENABLED_MODS_DEFAULT ""
#define CVAR_ENABLED_MODS_VALUE CVarGetString(CVAR_ENABLED_MODS_NAME, CVAR_ENABLED_MODS_DEFAULT)
#define CVAR_DISABLED_MODS_NAME "gSettings.DisabledMods"
#define CVAR_DISABLED_MODS_DEFAULT ""
#define CVAR_DISABLED_MODS_VALUE CVarGetString(CVAR_DISABLED_MODS_NAME, CVAR_DISABLED_MODS_DEFAULT)

// "|" was chosen as the separator due to
// it being an invalid character in NTFS
// and being rarely used in ext4
// it is also an ASCII character
// improving portability

// if being an ASCII character is not a requirement,
// other possible candidates include:
// - U+FFFF: non-character
// - any private use character
#define SEPARATOR "|"

void SetEnabledModsCVarValue() {
    auto joinMods = [](const std::vector<std::string>& mods) {
        std::string value;
        for (const auto& mod : mods) {
            value += mod + SEPARATOR;
        }
        if (!value.empty()) {
            value.pop_back();
        }
        return value;
    };

    const std::string enabled = joinMods(enabledModFiles);
    const std::string disabled = joinMods(disabledModFiles);
    CVarSetString(CVAR_ENABLED_MODS_NAME, enabled.c_str());
    CVarSetString(CVAR_DISABLED_MODS_NAME, disabled.c_str());
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

void AfterModChange() {
    // disabled mods are always sorted
    std::sort(disabledModFiles.begin(), disabledModFiles.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                            [](char c1, char c2) { return std::tolower(c1) < std::tolower(c2); });
    });
}

void RebuildModFileOrder(const std::vector<ModGroup>& groups, std::vector<std::string>& modFiles) {
    const std::set<std::string> previouslyEnabled(modFiles.begin(), modFiles.end());
    modFiles.clear();
    for (auto group = groups.rbegin(); group != groups.rend(); ++group) {
        for (auto file = group->files.rbegin(); file != group->files.rend(); ++file) {
            modFiles.push_back(*file);
        }
        for (const auto& variantGroup : group->variantGroups) {
            for (const auto& option : variantGroup.options) {
                for (auto file = option.files.rbegin(); file != option.files.rend(); ++file) {
                    if (previouslyEnabled.contains(*file)) {
                        modFiles.push_back(*file);
                    }
                }
            }
        }
        for (const auto& optionalGroup : group->optionalGroups) {
            for (auto file = optionalGroup.files.rbegin(); file != optionalGroup.files.rend(); ++file) {
                if (previouslyEnabled.contains(*file)) {
                    modFiles.push_back(*file);
                }
            }
        }
    }
}

void ModsPostDragAndDrop(std::vector<ModGroup>& groups, std::vector<std::string>& modFiles) {
    if (dragTargetIndex != SIZE_MAX && dragSourceIndex != SIZE_MAX && dragSourceIndex < groups.size() &&
        dragTargetIndex < groups.size()) {
        ModGroup group = std::move(groups[dragSourceIndex]);
        groups.erase(groups.begin() + dragSourceIndex);
        groups.insert(groups.begin() + dragTargetIndex, std::move(group));
        RebuildModFileOrder(groups, modFiles);
        AfterModChange();
    }
    dragTargetIndex = dragSourceIndex = SIZE_MAX;
}

void ModsHandleDragAndDrop(size_t targetIndex, const std::string& itemName,
                           ImGuiDragDropFlags flags = ImGuiDragDropFlags_SourceAllowNullID) {
    if (ImGui::BeginDragDropSource(flags)) {
        const size_t sourceIndex = targetIndex;
        ImGui::SetDragDropPayload("DragMove", &sourceIndex, sizeof(sourceIndex));
        ImGui::Text("Move %s", itemName.c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DragMove")) {
            IM_ASSERT(payload->DataSize == sizeof(size_t));
            dragSourceIndex = *static_cast<const size_t*>(payload->Data);
            dragTargetIndex = targetIndex;
        }
        ImGui::EndDragDropTarget();
    }
}

std::vector<std::string> GetEnabledModsFromCVar() {
    std::string enabledModsCVarValue = CVAR_ENABLED_MODS_VALUE;
    if (enabledModsCVarValue.empty())
        return {};
    return StringHelper::Split(enabledModsCVarValue, SEPARATOR);
}

std::vector<std::string> GetDisabledModsFromCVar() {
    std::string disabledModsCVarValue = CVAR_DISABLED_MODS_VALUE;
    if (disabledModsCVarValue.empty())
        return {};
    return StringHelper::Split(disabledModsCVarValue, SEPARATOR);
}

std::vector<std::string>& GetModFiles(bool enabled) {
    return enabled ? enabledModFiles : disabledModFiles;
}

std::shared_ptr<Ship::ArchiveManager> GetArchiveManager() {
    return Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager();
}

bool IsValidExtension(std::string extension) {
    if (
#ifdef INCLUDE_MPQ_SUPPORT
        // .mpq doesn't make sense to support because all tools to make such mods output OTR
        StringHelper::IEquals(extension, ".otr") /*|| StringHelper::IEquals(extension, ".mpq")*/ ||
#endif
        // .zip needs to be excluded because mods are most often distributed in zip archives
        // and thus could contain .otr/o2r files
        StringHelper::IEquals(extension, ".o2r") || StringHelper::IEquals(extension, ".pak")) {
        return true;
    }
    return false;
}

static bool IsOptionalFolderName(std::string folder) {
    std::transform(folder.begin(), folder.end(), folder.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return folder.find("optional") != std::string::npos || folder.find("opcional") != std::string::npos ||
           folder.find("bonus") != std::string::npos || folder.find("additional") != std::string::npos ||
           folder.find("alternate") != std::string::npos || folder.find("add-on") != std::string::npos ||
           folder.find("addon") != std::string::npos || folder == "dlc" || folder == "extra" || folder == "extras" ||
           folder.rfind("extra ", 0) == 0 || folder.rfind("extra_", 0) == 0 || folder.rfind("extra-", 0) == 0;
}

static bool IsOptionalModArchive(const std::filesystem::path& archivePath) {
    std::error_code relativePathError;
    const auto relativePath = std::filesystem::relative(archivePath, modsDirectory, relativePathError);
    if (relativePathError || relativePath.empty() || *relativePath.begin() == "..") {
        return false;
    }
    const auto relativeParent = relativePath.parent_path();
    auto component = relativeParent.begin();
    if (component != relativeParent.end()) {
        ++component;
    }
    for (; component != relativeParent.end(); ++component) {
        if (IsOptionalFolderName((*component).generic_string())) {
            return true;
        }
    }
    return false;
}

static bool recoveryChecked = false;
static bool skipModsThisRun = false;
static bool archivesAdded = false;
static std::set<std::string> loadedModArchives;
static bool editing = false;

static void CheckAndroidRecoveryMode() {
#if defined(__ANDROID__)
    if (recoveryChecked) {
        return;
    }
    recoveryChecked = true;

    std::filesystem::path marker =
        std::filesystem::path(Ship::Context::GetAppDirectoryPath(appShortName)) / ".android_safe_mode";
    if (std::filesystem::exists(marker)) {
        skipModsThisRun = true;
        std::error_code error;
        std::filesystem::remove(marker, error);
        SPDLOG_WARN("Previous Android startup failed; skipping mod archives for this launch");
    }
#endif
}

void UpdateModFiles(bool init = false, bool reset = false) {
    if (init) {
        CheckAndroidRecoveryMode();
    }
    if (init || reset) {
        enabledModFiles.clear();
        enabledModFiles = GetEnabledModsFromCVar();
        disabledModFiles = GetDisabledModsFromCVar();
    }
    unsupportedFiles.clear();
    filePaths.clear();
    bool changed = false;
    std::string modsPath = Ship::Context::LocateFileAcrossAppDirs("mods", appShortName);
    modsDirectory = modsPath.empty() ? std::filesystem::path() : std::filesystem::path(modsPath).lexically_normal();
    if (init && !modsDirectory.empty() && std::filesystem::is_directory(modsDirectory)) {
        std::vector<std::filesystem::path> looseModFiles;
        for (const auto& entry : std::filesystem::directory_iterator(modsDirectory)) {
            if (entry.is_regular_file() && IsValidExtension(entry.path().extension().string())) {
                looseModFiles.push_back(entry.path());
            }
        }
        for (const auto& modFile : looseModFiles) {
            std::string folderName = modFile.stem().string();
            if (folderName.empty() || folderName == "." || folderName == "..") {
                folderName = "mod-" + std::to_string(std::hash<std::string>{}(modFile.filename().string()));
            }
            const std::filesystem::path modFolder = modsDirectory / folderName;
            std::error_code filesystemError;
            std::filesystem::create_directories(modFolder, filesystemError);
            if (filesystemError) {
                SPDLOG_WARN("Could not create mod folder {}: {}", modFolder.string(), filesystemError.message());
                continue;
            }

            const std::filesystem::path organizedPath = modFolder / modFile.filename();
            if (std::filesystem::exists(organizedPath, filesystemError)) {
                if (filesystemError) {
                    SPDLOG_WARN("Could not inspect mod destination {}: {}", organizedPath.string(),
                                filesystemError.message());
                } else {
                    SPDLOG_WARN("Leaving loose mod {} in place because {} already exists", modFile.string(),
                                organizedPath.string());
                }
                continue;
            }

            std::filesystem::rename(modFile, organizedPath, filesystemError);
            if (filesystemError) {
                SPDLOG_WARN("Could not move mod {} into its folder: {}", modFile.string(), filesystemError.message());
            } else {
                SPDLOG_INFO("Organized mod {} into {}", modFile.filename().string(), modFolder.string());
            }
        }
    }
    if (modsPath.length() > 0 && std::filesystem::exists(modsPath)) {
        if (std::filesystem::is_directory(modsPath)) {
            for (const std::filesystem::directory_entry& p : std::filesystem::recursive_directory_iterator(
                     modsPath, std::filesystem::directory_options::follow_directory_symlink)) {
                if (p.is_directory()) {
                    continue;
                }
                std::string extension = p.path().extension().generic_string();
                if (!IsValidExtension(extension)) {
                    continue;
                }
                std::error_code relativePathError;
                const auto relativePath = std::filesystem::relative(p.path(), modsDirectory, relativePathError);
                if (relativePathError || relativePath.empty() || *relativePath.begin() == "..") {
                    SPDLOG_WARN("Could not resolve mod archive {} relative to {}: {}", p.path().string(),
                                modsDirectory.string(),
                                relativePathError ? relativePathError.message() : "path is outside the mods folder");
                    continue;
                }
                const std::string fileId = relativePath.lexically_normal().generic_string();
                filePaths.emplace(fileId, p.path());
            }

            auto migrateModList = [&](std::vector<std::string>& mods) {
                std::vector<std::string> migrated;
                std::set<std::string> seen;
                for (const auto& mod : mods) {
                    if (filePaths.contains(mod)) {
                        if (seen.insert(mod).second) {
                            migrated.push_back(mod);
                        }
                        continue;
                    }

                    bool found = false;
                    for (const auto& [fileId, path] : filePaths) {
                        if (path.stem().generic_string() == mod) {
                            found = true;
                            if (seen.insert(fileId).second) {
                                migrated.push_back(fileId);
                            }
                        }
                    }
                    if (!found) {
                        migrated.push_back(mod);
                    } else {
                        changed = true;
                    }
                }
                if (migrated != mods) {
                    mods = std::move(migrated);
                    changed = true;
                }
            };
            migrateModList(enabledModFiles);
            migrateModList(disabledModFiles);

            for (const auto& [fileId, path] : filePaths) {
                const bool enabled =
                    std::find(enabledModFiles.begin(), enabledModFiles.end(), fileId) != enabledModFiles.end();
                const bool disabled =
                    std::find(disabledModFiles.begin(), disabledModFiles.end(), fileId) != disabledModFiles.end();
                if (!enabled && !disabled) {
                    auto& targetList = IsOptionalModArchive(path) ? disabledModFiles : enabledModFiles;
                    targetList.push_back(fileId);
                    changed = true;
                }
            }
            ApplyVariantDefaults(changed);

            std::erase_if(enabledModFiles, [&](const std::string& mod) {
                if (filePaths.contains(mod)) {
                    return false;
                }
                changed = true;
                return true;
            });
            std::erase_if(disabledModFiles, [&](const std::string& mod) {
                if (filePaths.contains(mod)) {
                    return false;
                }
                changed = true;
                return true;
            });
        }
        if (changed) {
            SetEnabledModsCVarValue();
        }
    }
}

extern "C" void gfx_texture_cache_clear();

void EnableMod(std::string file) {
    disabledModFiles.erase(std::find(disabledModFiles.begin(), disabledModFiles.end(), file));
    enabledModFiles.insert(enabledModFiles.begin(), file);

    // TODO: runtime changes
    // GetArchiveManager()->AddArchive(file);
    AfterModChange();
}

void DisableMod(std::string file) {
    enabledModFiles.erase(std::find(enabledModFiles.begin(), enabledModFiles.end(), file));
    disabledModFiles.insert(disabledModFiles.begin(), file);

    // TODO: runtime changes
    // GetArchiveManager()->RemoveArchive(file);
    AfterModChange();
}

void DrawModInfo(std::string file) {
    ImGui::SameLine();
    ImGui::Text("%s", file.c_str());
}

std::string GetModDisplayName(const std::string& file) {
    const std::filesystem::path& archivePath = filePaths.at(file);
    std::error_code relativePathError;
    const auto relativePath = std::filesystem::relative(archivePath, modsDirectory, relativePathError);
    if (relativePathError || relativePath.empty() || *relativePath.begin() == "..") {
        return archivePath.stem().generic_string();
    }
    if (relativePath.begin() == relativePath.end() || std::next(relativePath.begin()) == relativePath.end()) {
        return archivePath.stem().generic_string();
    }
    auto root = relativePath.begin();
    if (StringHelper::IEquals(root->string(), "Texture Packs") ||
        StringHelper::IEquals(root->string(), "Texture_Packs")) {
        ++root;
        if (root == relativePath.end()) {
            return archivePath.stem().generic_string();
        }
    }
    return root->generic_string();
}

static std::filesystem::path GetModRootDirectory(const std::filesystem::path& archivePath) {
    std::error_code relativePathError;
    const auto relativePath = std::filesystem::relative(archivePath, modsDirectory, relativePathError);
    if (relativePathError || relativePath.empty() || *relativePath.begin() == ".." ||
        relativePath.begin() == relativePath.end() ||
        std::next(relativePath.begin()) == relativePath.end()) {
        return {};
    }
    auto root = relativePath.begin();
    if (StringHelper::IEquals(root->string(), "Texture Packs") ||
        StringHelper::IEquals(root->string(), "Texture_Packs")) {
        ++root;
        if (root == relativePath.end()) {
            return {};
        }
    }
    return modsDirectory / *root;
}

static void DeleteModContents(std::filesystem::path modRoot, const std::vector<std::string>& files) {
    std::error_code filesystemError;
    bool removedAny = false;
    if (!modRoot.empty()) {
        const auto relativeRoot = std::filesystem::relative(modRoot, modsDirectory, filesystemError);
        if (filesystemError || relativeRoot.empty() || relativeRoot == "." || *relativeRoot.begin() == ".." ||
            relativeRoot.begin() == relativeRoot.end()) {
            SPDLOG_ERROR("Refusing to delete mod directory outside the mods folder: {}", modRoot.string());
            return;
        }

        std::filesystem::remove_all(modRoot, filesystemError);
        if (filesystemError) {
            SPDLOG_ERROR("Could not delete mod directory {}: {}", modRoot.string(), filesystemError.message());
            return;
        }
        removedAny = true;
    } else {
        for (const auto& file : files) {
            const auto archivePath = filePaths.find(file);
            if (archivePath == filePaths.end()) {
                continue;
            }
            const auto relativePath = std::filesystem::relative(archivePath->second, modsDirectory, filesystemError);
            if (filesystemError || relativePath.empty() || relativePath == "." || *relativePath.begin() == ".." ||
                std::next(relativePath.begin()) != relativePath.end()) {
                SPDLOG_ERROR("Refusing to delete mod archive outside the mods folder: {}", archivePath->second.string());
                continue;
            }
            std::filesystem::remove(archivePath->second, filesystemError);
            if (filesystemError) {
                SPDLOG_ERROR("Could not delete mod archive {}: {}", archivePath->second.string(),
                             filesystemError.message());
                filesystemError.clear();
                continue;
            }
            removedAny = true;
        }
    }

    if (removedAny) {
        UpdateModFiles(false, false);
    }
}

static std::string GetOptionalGroupName(const std::filesystem::path& archivePath) {
    std::error_code relativePathError;
    const auto relativePath = std::filesystem::relative(archivePath, modsDirectory, relativePathError);
    if (relativePathError || relativePath.empty() || *relativePath.begin() == "..") {
        return {};
    }
    const auto relativeParent = relativePath.parent_path();
    auto component = relativeParent.begin();
    if (component == relativeParent.end()) {
        return {};
    }
    ++component;
    for (; component != relativeParent.end(); ++component) {
        std::string folder = component->generic_string();
        std::string normalized = folder;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        const bool optional = IsOptionalFolderName(normalized);
        if (!optional) {
            continue;
        }
        if (normalized == "_optional" || normalized == "optional" || normalized == "optionals" ||
            normalized == "opcional" || normalized == "opcionales") {
            const auto optionName = std::next(component);
            return optionName == relativeParent.end() ? folder : (*optionName).generic_string();
        }
        return folder;
    }
    return {};
}

static std::string JoinPathComponents(const std::filesystem::path& path) {
    std::string result;
    for (const auto& component : path) {
        if (!result.empty()) {
            result += " / ";
        }
        result += component.generic_string();
    }
    return result;
}

static bool IsDefaultVariantName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    for (const std::string_view marker : { "vanilla", "default", "original", "originale" }) {
        if (name.find(marker) != std::string::npos) {
            return true;
        }
    }
    if (name.rfind("base", 0) == 0 || name.find(" base") != std::string::npos ||
        name.find("_base") != std::string::npos || name.find("-base") != std::string::npos) {
        return true;
    }
    std::string word;
    for (const unsigned char character : name) {
        if (std::isalnum(character)) {
            word += static_cast<char>(character);
        } else {
            if (word == "og") {
                return true;
            }
            word.clear();
        }
    }
    if (word == "og" || name.rfind("og", 0) == 0) {
        return true;
    }
    return false;
}

static std::vector<ModGroup::VariantGroup> GetVariantGroupsForRoot(const std::filesystem::path& modRoot) {
    std::map<std::string, std::map<std::string, std::vector<std::string>>> candidates;
    for (const auto& [file, archivePath] : filePaths) {
        if (GetModRootDirectory(archivePath) != modRoot || !GetOptionalGroupName(archivePath).empty()) {
            continue;
        }

        std::error_code relativePathError;
        const auto relativePath = std::filesystem::relative(archivePath, modRoot, relativePathError);
        if (relativePathError || relativePath.empty() || *relativePath.begin() == "..") {
            continue;
        }
        const auto relativeParent = relativePath.parent_path();
        if (relativeParent.empty()) {
            continue;
        }

        std::filesystem::path categoryPath;
        for (const auto& component : relativeParent) {
            candidates[categoryPath.generic_string()][component.generic_string()].push_back(file);
            categoryPath /= component;
        }
    }

    std::set<std::string> validCategories;
    for (const auto& [category, options] : candidates) {
        if (options.size() >= 2) {
            validCategories.insert(category);
        }
    }

    std::vector<ModGroup::VariantGroup> result;
    for (auto& [category, options] : candidates) {
        if (options.size() < 2) {
            continue;
        }
        bool containsNestedCategories = false;
        for (const auto& [optionName, files] : options) {
            const std::string optionPath =
                category.empty() ? optionName : (std::filesystem::path(category) / optionName).generic_string();
            for (const auto& candidateCategory : validCategories) {
                if (candidateCategory == optionPath || candidateCategory.rfind(optionPath + "/", 0) == 0) {
                    containsNestedCategories = true;
                    break;
                }
            }
            if (containsNestedCategories) {
                break;
            }
        }
        if (containsNestedCategories) {
            continue;
        }

        ModGroup::VariantGroup group;
        group.name = category.empty() ? BenGui::TranslateUiText("Variants") : JoinPathComponents(category);
        for (auto& [name, files] : options) {
            std::sort(files.begin(), files.end());
            files.erase(std::unique(files.begin(), files.end()), files.end());
            group.options.push_back({ name, std::move(files) });
        }
        result.push_back(std::move(group));
    }
    return result;
}

static const ModGroup::VariantOption* GetDefaultVariantOption(const ModGroup::VariantGroup& group) {
    const auto option = std::find_if(group.options.begin(), group.options.end(),
                                     [](const ModGroup::VariantOption& item) { return IsDefaultVariantName(item.name); });
    return option == group.options.end() ? (group.options.empty() ? nullptr : &group.options.front()) : &*option;
}

static const std::string* GetActiveVariantFile(const ModGroup::VariantGroup& group) {
    for (const auto& option : group.options) {
        for (const auto& file : option.files) {
            if (std::find(enabledModFiles.begin(), enabledModFiles.end(), file) != enabledModFiles.end()) {
                return &file;
            }
        }
    }
    return nullptr;
}

static const ModGroup::VariantOption* GetVariantFolderSelection(const ModGroup::VariantGroup& group,
                                                                 const std::string& selectionKey) {
    const auto selectedFolder = selectedVariantFolders.find(selectionKey);
    if (selectedFolder != selectedVariantFolders.end()) {
        const auto option = std::find_if(group.options.begin(), group.options.end(),
                                         [&](const ModGroup::VariantOption& item) {
                                             return item.name == selectedFolder->second;
                                         });
        if (option != group.options.end()) {
            return &*option;
        }
    }

    if (const auto* activeFile = GetActiveVariantFile(group); activeFile != nullptr) {
        const auto activeOption = std::find_if(group.options.begin(), group.options.end(),
                                               [activeFile](const ModGroup::VariantOption& option) {
                                                   return std::find(option.files.begin(), option.files.end(),
                                                                    *activeFile) != option.files.end();
                                               });
        if (activeOption != group.options.end()) {
            return &*activeOption;
        }
    }
    return GetDefaultVariantOption(group);
}

static void ApplyVariantSelection(const std::vector<std::string>& filesToEnable,
                                  const std::vector<std::string>& filesToDisable) {
    size_t insertionIndex = enabledModFiles.size();
    for (size_t i = 0; i < enabledModFiles.size(); ++i) {
        if (std::find(filesToDisable.begin(), filesToDisable.end(), enabledModFiles[i]) != filesToDisable.end()) {
            insertionIndex = std::min(insertionIndex, i);
        }
    }

    std::erase_if(enabledModFiles, [&](const std::string& file) {
        return std::find(filesToDisable.begin(), filesToDisable.end(), file) != filesToDisable.end();
    });

    for (const auto& file : filesToDisable) {
        if (std::find(filesToEnable.begin(), filesToEnable.end(), file) != filesToEnable.end()) {
            continue;
        }
        if (std::find(disabledModFiles.begin(), disabledModFiles.end(), file) == disabledModFiles.end()) {
            disabledModFiles.push_back(file);
        }
    }

    std::vector<std::string> newlyEnabled;
    for (const auto& file : filesToEnable) {
        std::erase(disabledModFiles, file);
        if (std::find(enabledModFiles.begin(), enabledModFiles.end(), file) == enabledModFiles.end()) {
            newlyEnabled.push_back(file);
        }
    }
    insertionIndex = std::min(insertionIndex, enabledModFiles.size());
    enabledModFiles.insert(enabledModFiles.begin() + insertionIndex, newlyEnabled.begin(), newlyEnabled.end());
    AfterModChange();
    SetEnabledModsCVarValue();
}

static void ApplyVariantDefaults(bool& changed) {
    std::set<std::filesystem::path> modRoots;
    for (const auto& [file, archivePath] : filePaths) {
        const auto root = GetModRootDirectory(archivePath);
        if (!root.empty()) {
            modRoots.insert(root);
        }
    }

    for (const auto& root : modRoots) {
        for (const auto& variantGroup : GetVariantGroupsForRoot(root)) {
            std::vector<std::string> activeFiles;
            for (const auto& option : variantGroup.options) {
                for (const auto& file : option.files) {
                    if (std::find(enabledModFiles.begin(), enabledModFiles.end(), file) != enabledModFiles.end()) {
                        activeFiles.push_back(file);
                    }
                }
            }
            if (activeFiles.empty()) {
                continue;
            }

            std::string selectedFile;
            if (activeFiles.size() == 1) {
                selectedFile = activeFiles.front();
            } else if (const auto* defaultOption = GetDefaultVariantOption(variantGroup);
                       defaultOption != nullptr && !defaultOption->files.empty()) {
                selectedFile = defaultOption->files.front();
            } else {
                selectedFile = activeFiles.front();
            }
            if (selectedFile.empty()) {
                continue;
            }

            for (const auto& option : variantGroup.options) {
                for (const auto& file : option.files) {
                    if (file == selectedFile) {
                        const auto disabled = std::find(disabledModFiles.begin(), disabledModFiles.end(), file);
                        if (disabled != disabledModFiles.end()) {
                            disabledModFiles.erase(disabled);
                            changed = true;
                        }
                        if (std::find(enabledModFiles.begin(), enabledModFiles.end(), file) == enabledModFiles.end()) {
                            enabledModFiles.push_back(file);
                            changed = true;
                        }
                    } else {
                        const auto enabled = std::find(enabledModFiles.begin(), enabledModFiles.end(), file);
                        if (enabled != enabledModFiles.end()) {
                            enabledModFiles.erase(enabled);
                            changed = true;
                        }
                        if (std::find(disabledModFiles.begin(), disabledModFiles.end(), file) == disabledModFiles.end()) {
                            disabledModFiles.push_back(file);
                            changed = true;
                        }
                    }
                }
            }
        }
    }
}

std::vector<ModGroup> GetModGroups(const std::vector<std::string>& modFiles) {
    std::vector<ModGroup> groups;
    std::map<std::string, size_t> groupIndexes;
    std::map<std::string, std::vector<ModGroup::VariantGroup>> variantsByRoot;

    for (auto file = modFiles.rbegin(); file != modFiles.rend(); ++file) {
        const std::filesystem::path& archivePath = filePaths.at(*file);
        if (!GetOptionalGroupName(archivePath).empty()) {
            continue;
        }
        const std::filesystem::path modRoot = GetModRootDirectory(archivePath);
        const std::string key = modRoot.empty() ? "file:" + *file : "directory:" + modRoot.generic_string();
        auto variantGroups = variantsByRoot.find(key);
        if (variantGroups == variantsByRoot.end()) {
            variantGroups = variantsByRoot.emplace(key, modRoot.empty() ? std::vector<ModGroup::VariantGroup>{}
                                                                       : GetVariantGroupsForRoot(modRoot))
                                .first;
        }
        const bool isVariant = std::any_of(
            variantGroups->second.begin(), variantGroups->second.end(), [&](const ModGroup::VariantGroup& variantGroup) {
                return std::any_of(variantGroup.options.begin(), variantGroup.options.end(),
                                   [&](const ModGroup::VariantOption& option) {
                                       return std::find(option.files.begin(), option.files.end(), *file) !=
                                              option.files.end();
                                   });
            });
        const bool hasEnabledVariant = std::any_of(
            variantGroups->second.begin(), variantGroups->second.end(), [](const ModGroup::VariantGroup& variantGroup) {
                return std::any_of(variantGroup.options.begin(), variantGroup.options.end(),
                                   [](const ModGroup::VariantOption& option) {
                                       return std::any_of(option.files.begin(), option.files.end(),
                                                          [](const std::string& variantFile) {
                                                              return std::find(enabledModFiles.begin(),
                                                                               enabledModFiles.end(), variantFile) !=
                                                                     enabledModFiles.end();
                                                          });
                                   });
            });
        if (&modFiles == &disabledModFiles && isVariant && hasEnabledVariant) {
            continue;
        }

        auto [groupIndex, inserted] = groupIndexes.emplace(key, groups.size());
        if (inserted) {
            groups.push_back({ key, GetModDisplayName(*file), {} });
            groups.back().variantGroups = variantGroups->second;
        }

        if (!isVariant) {
            groups[groupIndex->second].files.push_back(*file);
        }
    }

    for (const auto& [file, archivePath] : filePaths) {
        const std::string optionName = GetOptionalGroupName(archivePath);
        if (optionName.empty()) {
            continue;
        }
        const std::filesystem::path modRoot = GetModRootDirectory(archivePath);
        if (modRoot.empty()) {
            continue;
        }
        const auto groupIndex = groupIndexes.find("directory:" + modRoot.generic_string());
        if (groupIndex == groupIndexes.end()) {
            continue;
        }
        auto& optionalGroups = groups[groupIndex->second].optionalGroups;
        auto option = std::find_if(optionalGroups.begin(), optionalGroups.end(),
                                   [&](const ModGroup::OptionalGroup& item) { return item.name == optionName; });
        if (option == optionalGroups.end()) {
            optionalGroups.push_back({ optionName, {} });
            option = std::prev(optionalGroups.end());
        }
        option->files.push_back(file);
    }

    return groups;
}

static bool MatchesModSearch(const ModGroup& group, const std::string& query) {
    if (query.empty()) {
        return true;
    }

    auto containsQuery = [&query](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        std::string normalizedQuery = query;
        std::transform(normalizedQuery.begin(), normalizedQuery.end(), normalizedQuery.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return value.find(normalizedQuery) != std::string::npos;
    };

    if (containsQuery(group.name)) {
        return true;
    }
    if (std::any_of(group.files.begin(), group.files.end(), containsQuery)) {
        return true;
    }
    if (std::any_of(group.variantGroups.begin(), group.variantGroups.end(),
                    [&](const ModGroup::VariantGroup& variantGroup) {
                        return containsQuery(variantGroup.name) ||
                               std::any_of(variantGroup.options.begin(), variantGroup.options.end(),
                                           [&](const ModGroup::VariantOption& option) {
                                               return containsQuery(option.name) ||
                                                      std::any_of(option.files.begin(), option.files.end(),
                                                                  containsQuery);
                                           });
                    })) {
        return true;
    }
    return std::any_of(group.optionalGroups.begin(), group.optionalGroups.end(),
                       [&](const ModGroup::OptionalGroup& option) {
                           return containsQuery(option.name) ||
                                  std::any_of(option.files.begin(), option.files.end(), containsQuery);
                       });
}

void DrawMods(bool enabled) {
    std::vector<std::string>& selectedModFiles = GetModFiles(enabled);
    if (selectedModFiles.empty()) {
        ImGui::TextDisabled("%s", BenGui::TranslateUiText(enabled ? "No active mods yet." : "No disabled mods."));
        return;
    }

    std::vector<ModGroup> groups = GetModGroups(selectedModFiles);
    std::vector<std::vector<std::string>> toggledGroups;
    std::vector<std::pair<std::vector<std::string>, bool>> toggledOptionalGroups;
    std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> toggledVariants;
    size_t switchFromIndex = groups.size();
    size_t switchToIndex = groups.size();
    const size_t visibleCount = std::count_if(groups.begin(), groups.end(), [](const ModGroup& group) {
        return MatchesModSearch(group, modSearchQuery);
    });
    if (visibleCount == 0) {
        ImGui::TextDisabled("%s", BenGui::TranslateUiText("No mods match this search."));
        return;
    }

    for (size_t i = 0; i < groups.size(); i++) {
        const ModGroup& group = groups[i];
        if (!MatchesModSearch(group, modSearchQuery)) {
            continue;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              enabled ? IM_COL32(36, 47, 57, 245) : IM_COL32(34, 37, 45, 230));
        ImGui::BeginChild((group.key + "_card").c_str(), ImVec2(0.0f, 84.0f), true,
                          ImGuiWindowFlags_NoScrollbar);
        bool isEnabled = enabled;
        std::vector<std::string> modFilesToToggle = group.files;
        for (const auto& variantGroup : group.variantGroups) {
            const std::string* selectedFile = GetActiveVariantFile(variantGroup);
            if (selectedFile == nullptr) {
                const auto* defaultOption = GetDefaultVariantOption(variantGroup);
                if (defaultOption != nullptr && !defaultOption->files.empty()) {
                    selectedFile = &defaultOption->files.front();
                }
            }
            if (selectedFile != nullptr) {
                modFilesToToggle.push_back(*selectedFile);
            }
        }
        ImGui::PushID(group.key.c_str());
        if (ImGui::Checkbox("##mod-enabled", &isEnabled)) {
            toggledGroups.push_back(std::move(modFilesToToggle));
        }
        ImGui::PopID();

        // it's not relevant to reorder disabled mods
        if (enabled) {
            ImGui::SameLine();
            if (i == 0) {
                ImGui::BeginDisabled();
            }
            if (UIWidgets::StateButton((group.key + "_up").c_str(), ICON_FA_ARROW_UP, ImVec2(25, 25),
                                       UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
                switchFromIndex = i;
                switchToIndex = i - 1;
            }
            if (i == 0) {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();
            if (i == groups.size() - 1) {
                ImGui::BeginDisabled();
            }
            if (UIWidgets::StateButton((group.key + "_down").c_str(), ICON_FA_ARROW_DOWN, ImVec2(25, 25),
                                       UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
                switchFromIndex = i;
                switchToIndex = i + 1;
            }
            if (i == groups.size() - 1) {
                ImGui::EndDisabled();
            }
        }

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", group.name.c_str());
        size_t archiveCount = group.files.size();
        if (enabled) {
            for (const auto& variantGroup : group.variantGroups) {
                archiveCount += GetActiveVariantFile(variantGroup) != nullptr ? 1 : 0;
            }
        } else {
            for (const auto& variantGroup : group.variantGroups) {
                for (const auto& option : variantGroup.options) {
                    archiveCount += option.files.size();
                }
            }
        }
        if (enabled) {
            ImGui::TextDisabled("%zu archive%s  |  Priority %02d", archiveCount,
                                archiveCount == 1 ? "" : "s", static_cast<int>(i + 1));
        } else {
            ImGui::TextDisabled("%zu archive%s", archiveCount, archiveCount == 1 ? "" : "s");
        }
        ImGui::EndGroup();
        std::filesystem::path groupRoot;
        std::vector<std::string> filesToDelete = group.files;
        const auto findGroupRoot = [&](const std::vector<std::string>& files) {
            for (const auto& file : files) {
                if (std::find(filesToDelete.begin(), filesToDelete.end(), file) == filesToDelete.end()) {
                    filesToDelete.push_back(file);
                }
                const auto archivePath = filePaths.find(file);
                if (archivePath != filePaths.end()) {
                    groupRoot = GetModRootDirectory(archivePath->second);
                    if (!groupRoot.empty()) {
                        return;
                    }
                }
            }
        };
        findGroupRoot(group.files);
        for (const auto& variantGroup : group.variantGroups) {
            for (const auto& option : variantGroup.options) {
                findGroupRoot(option.files);
            }
        }
        for (const auto& optionalGroup : group.optionalGroups) {
            findGroupRoot(optionalGroup.files);
        }

        if (!group.optionalGroups.empty() || !group.variantGroups.empty() || !filesToDelete.empty()) {
            ImGui::SameLine();
            ImGui::PushID(group.key.c_str());
            const std::string modSettingsPopup =
                std::string(BenGui::TranslateUiText("Mod Settings")) + "###optional-mod-settings";
            if (!group.optionalGroups.empty() || !group.variantGroups.empty()) {
                if (ImGui::Button(BenGui::TranslateUiText("Mod Settings"))) {
                    ImGui::OpenPopup(modSettingsPopup.c_str());
                }
            }
            if (ImGui::IsPopupOpen(modSettingsPopup.c_str())) {
                ImGui::SetNextWindowSize(ImVec2(600.0f, 520.0f), ImGuiCond_Appearing);
            }
            if (ImGui::BeginPopupModal(modSettingsPopup.c_str(), nullptr,
                                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
                if (!group.variantGroups.empty()) {
                    for (size_t variantIndex = 0; variantIndex < group.variantGroups.size(); ++variantIndex) {
                        const auto& variantGroup = group.variantGroups[variantIndex];
                        const std::string selectionKey = group.key + "|" + variantGroup.name;
                        const auto* selectedFolder = GetVariantFolderSelection(variantGroup, selectionKey);
                        const char* preview = selectedFolder == nullptr ? "" : selectedFolder->name.c_str();
                        const std::string* activeFile = GetActiveVariantFile(variantGroup);
                        ImGui::PushID(static_cast<int>(variantIndex));
                        ImGui::TextUnformatted(variantGroup.name.c_str());
                        ImGui::Separator();
                        ImGui::SetNextItemWidth(-1.0f);
                        ImGui::BeginDisabled(!enabled);
                        if (ImGui::BeginCombo("##variant-choice", preview)) {
                            for (const auto& option : variantGroup.options) {
                                const bool isSelected = selectedFolder == &option;
                                if (ImGui::Selectable(option.name.c_str(), isSelected) && !isSelected) {
                                    selectedVariantFolders[selectionKey] = option.name;
                                    std::vector<std::string> filesToDisable;
                                    for (const auto& folder : variantGroup.options) {
                                        filesToDisable.insert(filesToDisable.end(), folder.files.begin(),
                                                              folder.files.end());
                                    }
                                    const std::vector<std::string> filesToEnable =
                                        option.files.empty() ? std::vector<std::string>{}
                                                             : std::vector<std::string>{ option.files.front() };
                                    toggledVariants.emplace_back(filesToEnable,
                                                                 std::move(filesToDisable));
                                }
                                if (isSelected) {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::EndDisabled();
                        if (selectedFolder != nullptr) {
                            ImGui::TextUnformatted(selectedFolder->name.c_str());
                            ImGui::BeginDisabled(!enabled);
                            ImGui::BeginChild("variant-files", ImVec2(0.0f, 150.0f), true);
                            for (const auto& file : selectedFolder->files) {
                                const auto archivePath = filePaths.find(file);
                                const std::string label =
                                    archivePath != filePaths.end() ? archivePath->second.stem().string()
                                                                  : std::filesystem::path(file).stem().string();
                                bool isSelected = activeFile != nullptr && *activeFile == file;
                                ImGui::PushID(file.c_str());
                                if (ImGui::Checkbox(label.c_str(), &isSelected) && isSelected) {
                                    std::vector<std::string> filesToDisable;
                                    for (const auto& folder : variantGroup.options) {
                                        filesToDisable.insert(filesToDisable.end(), folder.files.begin(),
                                                              folder.files.end());
                                    }
                                    toggledVariants.emplace_back(
                                        isSelected ? std::vector<std::string>{ file } : std::vector<std::string>{},
                                        std::move(filesToDisable));
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndChild();
                            ImGui::EndDisabled();
                        }
                        ImGui::Spacing();
                        ImGui::PopID();
                    }
                }
                if (!group.optionalGroups.empty() && !group.variantGroups.empty()) {
                    ImGui::Separator();
                }
                if (!group.optionalGroups.empty()) {
                    ImGui::Text("%s", BenGui::TranslateUiText("Optional content"));
                }
                for (const auto& optionalGroup : group.optionalGroups) {
                    bool optionEnabled = std::all_of(
                        optionalGroup.files.begin(), optionalGroup.files.end(), [](const std::string& file) {
                            return std::find(enabledModFiles.begin(), enabledModFiles.end(), file) !=
                                   enabledModFiles.end();
                        });
                    if (ImGui::Checkbox(optionalGroup.name.c_str(), &optionEnabled)) {
                        toggledOptionalGroups.emplace_back(optionalGroup.files, optionEnabled);
                    }
                }
                if (ImGui::Button(BenGui::TranslateUiText("Close"))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (!filesToDelete.empty()) {
                if (!group.optionalGroups.empty() || !group.variantGroups.empty()) {
                    ImGui::SameLine();
                }
                if (ImGui::Button(BenGui::TranslateUiText("Delete"))) {
                    const std::string prompt =
                        std::string(BenGui::TranslateUiText("Delete this mod and all its files?")) + "\n" + group.name;
                    BenGui::RegisterPopup(BenGui::TranslateUiText("Delete Mod"), prompt,
                                          BenGui::TranslateUiText("Delete"),
                                          BenGui::TranslateUiText("Cancel"),
                                          [modRoot = groupRoot, files = filesToDelete]() {
                                              DeleteModContents(modRoot, files);
                                          });
                }
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (enabled) {
            ModsHandleDragAndDrop(i, group.name);
        }
    }

    if (enabled) {
        ModsPostDragAndDrop(groups, selectedModFiles);
    }

    if (switchFromIndex < groups.size() && switchToIndex < groups.size()) {
        std::iter_swap(groups.begin() + switchFromIndex, groups.begin() + switchToIndex);
        RebuildModFileOrder(groups, selectedModFiles);
        AfterModChange();
    }

    for (const auto& groupFiles : toggledGroups) {
        for (const auto& file : groupFiles) {
            if (enabled) {
                DisableMod(file);
            } else {
                EnableMod(file);
            }
        }
    }
    for (const auto& [files, shouldEnable] : toggledOptionalGroups) {
        for (const auto& file : files) {
            if (shouldEnable) {
                EnableMod(file);
            } else {
                DisableMod(file);
            }
        }
    }
    for (const auto& [filesToEnable, filesToDisable] : toggledVariants) {
        ApplyVariantSelection(filesToEnable, filesToDisable);
    }
}

void ModMenuWindow::Draw() {
    if (!IsVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 20.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    GuiWindow::Draw();
    ImGui::PopStyleVar(2);
}

void ModMenuWindow::DrawElement() {
    const bool gameStarted = !BenGui_IsStartupMenuActive();
    const ImVec4 accent = UIWidgets::ColorValues.at(THEME_COLOR);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(27, 31, 40, 250));
    ImGui::BeginChild("mod-manager-hero", ImVec2(0.0f, 82.0f), true, ImGuiWindowFlags_NoScrollbar);
    if (UIWidgets::Button(BenGui::TranslateUiText("Back"),
                          UIWidgets::ButtonOptions().Size(ImVec2(112.0f, 40.0f)).Color(THEME_COLOR))) {
        Hide();
        if (BenGui_IsStartupMenuActive()) {
            if (BenGui::mBenMenu != nullptr) {
                BenGui::mBenMenu->Hide();
            }
        } else if (BenGui::mBenMenu != nullptr) {
            BenGui::mBenMenu->Show();
        }
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(accent, "%s", BenGui::TranslateUiText("Your Mod Library"));
    ImGui::TextDisabled("%s", gameStarted ? "Changes apply after restarting the game"
                                          : "Choose what to load, then start your game");
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 250.0f);
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.52f, 0.84f, 0.68f, 1.0f), "%zu  ACTIVE", enabledModFiles.size());
    ImGui::TextColored(ImVec4(0.68f, 0.72f, 0.81f, 1.0f), "%zu  INACTIVE", disabledModFiles.size());
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(29, 33, 42, 235));
    ImGui::BeginChild("mod-manager-toolbar", ImVec2(0.0f, 48.0f), true, ImGuiWindowFlags_NoScrollbar);
    BenGui::mBenMenu->MenuDrawItem(enableModsWidget, 200, THEME_COLOR);
    ImGui::SameLine();
    BenGui::mBenMenu->MenuDrawItem(tabHotkeyWidget, 200, THEME_COLOR);

    // if (UIWidgets::Button(
    //         "Update", UIWidgets::ButtonOptions({ { .disabled = editing, .disabledTooltip = "Currently editing..." }
    //         })
    //                       .Size(UIWidgets::Sizes::Inline)
    //                       .Color(THEME_COLOR))) {
    //     UpdateModFiles();
    // }
    // ImGui::SameLine();
    static bool editingInitialized = false;
    static bool previousGameStarted = false;
    if (!editingInitialized || gameStarted != previousGameStarted) {
        editing = !gameStarted;
        previousGameStarted = gameStarted;
        editingInitialized = true;
    }
    if (gameStarted) {
        if (UIWidgets::Button("Edit",
                              UIWidgets::ButtonOptions({ { .disabled = editing, .disabledTooltip = "Already editing..." } })
                                  .Size(UIWidgets::Sizes::Inline)
                                  .Color(THEME_COLOR))) {
            editing = true;
        }
    } else {
        editing = true;
    }
    if (editing) {
        if (gameStarted) {
            ImGui::SameLine();
            if (UIWidgets::Button("Cancel", UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline))) {
                editing = false;
                UpdateModFiles(false, true);
            }
            ImGui::SameLine();
        }
        if (UIWidgets::Button("Clear List", UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline))) {
            BenGui::RegisterPopup("Clear List",
                                  gameStarted
                                      ? "Clear the current mod list and force a rebuild on next boot.\nClick Apply & "
                                        "Close to save this change."
                                      : "Clear the current mod list and force a rebuild on next boot.\nClick Apply to "
                                        "save this change.",
                                  "Clear", "Cancel", [&]() {
                                      enabledModFiles.clear();
                                      AfterModChange();
                                  });
        }
        ImGui::SameLine();
        const char* applyButtonLabel = BenGui::TranslateUiText(gameStarted ? "Apply & Close" : "Apply");
        if (UIWidgets::Button(applyButtonLabel,
                              UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline).Color(THEME_COLOR))) {
            if (!gameStarted) {
                SetEnabledModsCVarValue();
                Ship::Context::GetRawInstance()->GetConsoleVariables()->Save();
                editing = false;
            } else {
                BenGui::RegisterPopup("Apply & Close",
                                      "Application currently requires a restart. Save the mod info and close 2S2H?",
                                      "Close", "Cancel", [&]() {
                                          SetEnabledModsCVarValue();
                                          Ship::Context::GetRawInstance()->GetConsoleVariables()->Save();
                                          Ship::Context::GetRawInstance()->GetWindow()->Close();
                                      });
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Spacing();

    std::set<std::string> previouslyEnabledMods(enabledModFiles.begin(), enabledModFiles.end());
    bool downloadedMod = false;
    if (ImGui::BeginTabBar("mods-manager-tabs")) {
        if (ImGui::BeginTabItem(BenGui::TranslateUiText("Installed Mods"))) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##mod-search", BenGui::TranslateUiText("Search installed mods"),
                                     modSearchQuery, sizeof(modSearchQuery));
            ImGui::BeginDisabled(!editing);
            if (ImGui::BeginChild("All Mods", ImVec2(0.0f, -8.0f), false)) {
                const float columnWidth =
                    (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));

                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(28, 38, 42, 220));
                ImGui::BeginChild("active-mods-panel", ImVec2(columnWidth, 0.0f), true);
                ImGui::TextColored(ImVec4(0.52f, 0.84f, 0.68f, 1.0f), "%s  %zu",
                                   BenGui::TranslateUiText("ACTIVE LOAD ORDER"), enabledModFiles.size());
                ImGui::TextDisabled("%s", BenGui::TranslateUiText("Higher priority overrides mods below."));
                ImGui::Separator();
                DrawMods(true);
                ImGui::EndChild();
                ImGui::PopStyleColor();

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(33, 35, 42, 220));
                ImGui::BeginChild("inactive-mods-panel", ImVec2(0.0f, 0.0f), true);
                ImGui::TextColored(ImVec4(0.68f, 0.72f, 0.81f, 1.0f), "%s  %zu",
                                   BenGui::TranslateUiText("DISABLED"), disabledModFiles.size());
                ImGui::TextDisabled("%s", BenGui::TranslateUiText("Available but not loaded."));
                ImGui::Separator();
                DrawMods(false);
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar(2);
            }
            ImGui::EndChild();
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(BenGui::TranslateUiText("Download Mods"))) {
            downloadedMod = ModCatalog::Draw(modsDirectory, gameStarted);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (downloadedMod) {
        UpdateModFiles(false, false);
        if (gameStarted) {
            for (const auto& mod : enabledModFiles) {
                if (!previouslyEnabledMods.contains(mod)) {
                    DisableMod(mod);
                }
            }
            SetEnabledModsCVarValue();
        }
    }
}

void ModMenu_LoadArchives() {
    UpdateModFiles(true);
}

static std::wstring QuoteWindowsArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            backslashes++;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted += L'"';
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            quoted += character;
            backslashes = 0;
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

static bool ConvertPakToArchives(const std::filesystem::path& pakPath,
                                 std::vector<std::filesystem::path>& convertedArchives,
                                 std::filesystem::path& cacheDirectory, std::string& error) {
    std::vector<wchar_t> modulePath(32768);
    const DWORD modulePathLength =
        GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (modulePathLength == 0 || modulePathLength >= modulePath.size()) {
        error = "Could not determine the game executable path.";
        return false;
    }

    const std::filesystem::path executablePath(std::wstring(modulePath.data(), modulePathLength));
    const std::filesystem::path converterPath =
        executablePath.parent_path() / "tools" / "ML64PakConverter.exe";
    if (!std::filesystem::is_regular_file(converterPath)) {
        error = "The ModLoader64 .pak converter is missing. Build it with tools\\build_ml64pak_converter.ps1.";
        return false;
    }

    const std::filesystem::path absolutePakPath = std::filesystem::absolute(pakPath).lexically_normal();
    const std::filesystem::path appDataPath =
        std::filesystem::absolute(Ship::Context::GetAppDirectoryPath(appShortName)).lexically_normal();
    const std::wstring sourceKey = absolutePakPath.wstring();
    const size_t sourceHash = std::hash<std::wstring>{}(sourceKey);
    std::wstringstream cacheName;
    cacheName << std::hex << sourceHash;
    cacheDirectory = appDataPath / "pak_cache" / cacheName.str();
    std::error_code filesystemError;
    std::filesystem::create_directories(cacheDirectory, filesystemError);
    if (filesystemError) {
        error = "Could not create the .pak conversion cache: " + filesystemError.message();
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(cacheDirectory, filesystemError)) {
        if (filesystemError) {
            error = "Could not inspect the .pak conversion cache: " + filesystemError.message();
            return false;
        }
        if (entry.is_regular_file(filesystemError) && StringHelper::IEquals(entry.path().extension().string(), ".o2r")) {
            std::filesystem::remove(entry.path(), filesystemError);
            if (filesystemError) {
                error = "Could not clear an old converted archive: " + filesystemError.message();
                return false;
            }
        }
    }
    if (filesystemError) {
        error = "Could not inspect the .pak conversion cache: " + filesystemError.message();
        return false;
    }

    const std::filesystem::path logPath = cacheDirectory / "conversion.log";
    SECURITY_ATTRIBUTES securityAttributes = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE logFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &securityAttributes, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logFile == INVALID_HANDLE_VALUE) {
        error = "Could not create the .pak conversion log.";
        return false;
    }

    const std::wstring commandLine = QuoteWindowsArgument(converterPath.wstring()) + L" --pak " +
                                     QuoteWindowsArgument(absolutePakPath.wstring()) + L" --out-dir " +
                                     QuoteWindowsArgument(cacheDirectory.wstring()) + L" --game MM";
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = logFile;
    startupInfo.hStdError = logFile;
    PROCESS_INFORMATION processInfo = {};
    const BOOL created = CreateProcessW(converterPath.c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, cacheDirectory.c_str(), &startupInfo, &processInfo);
    CloseHandle(logFile);
    if (!created) {
        error = "Could not start the .pak converter (Windows error " + std::to_string(GetLastError()) + ").";
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    if (waitResult != WAIT_OBJECT_0) {
        error = "Waiting for the .pak converter failed (Windows error " + std::to_string(GetLastError()) + ").";
        return false;
    }

    std::ifstream logStream(logPath, std::ios::binary);
    const std::string converterLog((std::istreambuf_iterator<char>(logStream)), std::istreambuf_iterator<char>());
    if (exitCode != 0) {
        error = "This .pak could not be converted. Only supported Majora's Mask ModLoader64 player-model and "
                "equipment content can be loaded.\n";
        error += converterLog.substr(converterLog.size() > 3000 ? converterLog.size() - 3000 : 0);
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(cacheDirectory, filesystemError)) {
        if (filesystemError) {
            error = "Could not read the converted .pak archives: " + filesystemError.message();
            return false;
        }
        if (entry.is_regular_file(filesystemError) &&
            StringHelper::IEquals(entry.path().extension().string(), ".o2r")) {
            convertedArchives.push_back(entry.path());
        }
    }
    if (filesystemError) {
        error = "Could not read the converted .pak archives: " + filesystemError.message();
        return false;
    }
    std::sort(convertedArchives.begin(), convertedArchives.end());
    if (convertedArchives.empty()) {
        error = "The .pak converter did not create any supported Majora's Mask archives in the conversion cache.";
        if (!converterLog.empty()) {
            error += "\n";
            error += converterLog.substr(converterLog.size() > 3000 ? converterLog.size() - 3000 : 0);
        }
        return false;
    }
    SPDLOG_INFO("Converted .pak {} into {} archive(s) under {}", absolutePakPath.string(), convertedArchives.size(),
                cacheDirectory.string());
    return true;
}

static bool FilesHaveSameContents(const std::filesystem::path& first, const std::filesystem::path& second) {
    std::error_code filesystemError;
    const uintmax_t firstSize = std::filesystem::file_size(first, filesystemError);
    if (filesystemError) {
        return false;
    }
    const uintmax_t secondSize = std::filesystem::file_size(second, filesystemError);
    if (filesystemError || firstSize != secondSize) {
        return false;
    }

    std::ifstream firstStream(first, std::ios::binary);
    std::ifstream secondStream(second, std::ios::binary);
    if (!firstStream || !secondStream) {
        return false;
    }

    std::array<char, 64 * 1024> firstBuffer{};
    std::array<char, 64 * 1024> secondBuffer{};
    while (firstStream && secondStream) {
        firstStream.read(firstBuffer.data(), firstBuffer.size());
        secondStream.read(secondBuffer.data(), secondBuffer.size());
        const std::streamsize firstRead = firstStream.gcount();
        const std::streamsize secondRead = secondStream.gcount();
        if (firstRead != secondRead || std::memcmp(firstBuffer.data(), secondBuffer.data(), firstRead) != 0) {
            return false;
        }
    }
    return firstStream.eof() && secondStream.eof();
}

static bool InstallConvertedArchives(const std::filesystem::path& pakPath,
                                     const std::vector<std::filesystem::path>& convertedArchives,
                                     std::vector<std::filesystem::path>& installedArchives, std::string& error) {
    const std::filesystem::path destinationDirectory =
        std::filesystem::absolute(pakPath.parent_path()).lexically_normal();

    for (const auto& convertedArchive : convertedArchives) {
        const std::string archiveStem = convertedArchive.stem().string();
        for (size_t suffix = 1;; ++suffix) {
            const std::string candidateName = archiveStem + (suffix == 1 ? "" : "_" + std::to_string(suffix));
            const std::filesystem::path destination = destinationDirectory / (candidateName + ".o2r");
            std::error_code filesystemError;
            const bool destinationExists = std::filesystem::exists(destination, filesystemError);
            if (filesystemError) {
                error = "Could not inspect the mods folder for a converted archive: " + filesystemError.message();
                return false;
            }

            if (destinationExists) {
                if (FilesHaveSameContents(convertedArchive, destination)) {
                    installedArchives.push_back(destination);
                    SPDLOG_INFO("Converted mod archive already exists at {}", destination.string());
                    break;
                }
                continue;
            }

            std::filesystem::copy_file(convertedArchive, destination, std::filesystem::copy_options::none,
                                       filesystemError);
            if (filesystemError) {
                error = "Could not move a converted archive into the mods folder: " + filesystemError.message();
                return false;
            }
            installedArchives.push_back(destination);
            SPDLOG_INFO("Installed converted mod archive {} from {}", destination.string(), convertedArchive.string());
            break;
        }
    }
    return true;
}

static void CleanupPakConversionCache(const std::filesystem::path& cacheDirectory) {
    if (cacheDirectory.empty()) {
        return;
    }

    std::error_code filesystemError;
    std::filesystem::remove_all(cacheDirectory, filesystemError);
    if (filesystemError) {
        SPDLOG_WARN("Could not remove temporary .pak conversion cache directory {}: {}", cacheDirectory.string(),
                    filesystemError.message());
        return;
    }

    const std::filesystem::path cacheRoot = cacheDirectory.parent_path();
    const bool cacheRootEmpty = std::filesystem::is_empty(cacheRoot, filesystemError);
    if (filesystemError) {
        SPDLOG_WARN("Could not inspect .pak conversion cache root {}: {}", cacheRoot.string(),
                    filesystemError.message());
    } else if (cacheRootEmpty) {
        std::filesystem::remove(cacheRoot, filesystemError);
        if (filesystemError) {
            SPDLOG_WARN("Could not remove empty .pak conversion cache root {}: {}", cacheRoot.string(),
                        filesystemError.message());
        }
    }
}

static void RemovePreviousPakConversionArchives(const std::filesystem::path& pakPath) {
    const std::wstring sourceKey = std::filesystem::absolute(pakPath).lexically_normal().wstring();
    std::ostringstream prefixStream;
    prefixStream << "pakconv_" << std::hex << std::hash<std::wstring>{}(sourceKey) << "_";
    const std::string namePrefix = prefixStream.str();
    std::error_code filesystemError;
    for (const auto& entry : std::filesystem::directory_iterator(pakPath.parent_path(), filesystemError)) {
        if (filesystemError) {
            SPDLOG_WARN("Could not inspect old converted archives: {}", filesystemError.message());
            return;
        }
        const std::string filename = entry.path().filename().string();
        if (entry.is_regular_file(filesystemError) && filename.rfind(namePrefix, 0) == 0 &&
            StringHelper::IEquals(entry.path().extension().string(), ".o2r")) {
            std::filesystem::remove(entry.path(), filesystemError);
            if (filesystemError) {
                SPDLOG_WARN("Could not remove previous converted archive {}: {}", entry.path().string(),
                            filesystemError.message());
                filesystemError.clear();
            }
        }
    }
    if (filesystemError) {
        SPDLOG_WARN("Could not inspect old converted archives: {}", filesystemError.message());
    }
}

static bool RemoveLegacyOotArchives(const std::filesystem::path& pakPath, std::string& error) {
    const std::string legacyPrefix = pakPath.stem().string() + "_";
    std::error_code filesystemError;
    for (const auto& entry : std::filesystem::directory_iterator(pakPath.parent_path(), filesystemError)) {
        if (filesystemError) {
            error = "Could not inspect old converted archives: " + filesystemError.message();
            return false;
        }
        const std::string filename = entry.path().filename().string();
        if (!entry.is_regular_file(filesystemError) || filename.rfind(legacyPrefix, 0) != 0 ||
            filename.find("_OOT_") == std::string::npos ||
            !StringHelper::IEquals(entry.path().extension().string(), ".o2r")) {
            continue;
        }
        std::filesystem::remove(entry.path(), filesystemError);
        if (filesystemError) {
            error = "Could not remove an obsolete OoT-only archive generated from this .pak: " +
                    filesystemError.message();
            return false;
        }
    }
    if (filesystemError) {
        error = "Could not inspect old converted archives: " + filesystemError.message();
        return false;
    }
    return true;
}

static bool IsTexturePackArchive(const std::filesystem::path& archivePath) {
    std::error_code relativePathError;
    const std::filesystem::path relativePath = std::filesystem::relative(archivePath, modsDirectory, relativePathError);
    if (relativePathError || relativePath.empty() || *relativePath.begin() == "..") {
        return false;
    }
    std::string category = relativePath.begin()->string();
    std::transform(category.begin(), category.end(), category.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return category == "texture packs" || category == "texture_packs";
}

bool ModMenu_StartGame(std::string& error) {
    if (archivesAdded || skipModsThisRun) {
        return true;
    }

    bool convertedPak = false;
    bool removedConvertedPak = false;
    const auto archiveManager = GetArchiveManager();
    if (archiveManager == nullptr) {
        error = "The resource archive manager is unavailable.";
        return false;
    }

    std::vector<std::string> modLoadOrder = enabledModFiles;
    std::stable_partition(modLoadOrder.begin(), modLoadOrder.end(), [](const std::string& mod) {
        const auto archivePath = filePaths.find(mod);
        return archivePath != filePaths.end() && IsTexturePackArchive(archivePath->second);
    });
    for (const std::string& mod : modLoadOrder) {
        const auto archivePath = filePaths.find(mod);
        if (archivePath == filePaths.end()) {
            SPDLOG_WARN("Skipping enabled mod {} because its archive was not found", mod);
            continue;
        }

        std::vector<std::filesystem::path> archivesToLoad;
        const bool isPak = StringHelper::IEquals(archivePath->second.extension().string(), ".pak");
        std::vector<std::filesystem::path> convertedArchives;
        std::filesystem::path conversionCache;
        if (isPak) {
            if (!ConvertPakToArchives(archivePath->second, convertedArchives, conversionCache, error)) {
                CleanupPakConversionCache(conversionCache);
                error = archivePath->second.filename().string() + ": " + error;
                return false;
            }
            if (!InstallConvertedArchives(archivePath->second, convertedArchives, archivesToLoad, error)) {
                CleanupPakConversionCache(conversionCache);
                error = archivePath->second.filename().string() + ": " + error;
                return false;
            }
            CleanupPakConversionCache(conversionCache);
            convertedPak = true;
        } else {
            archivesToLoad.push_back(archivePath->second);
        }

        for (const auto& archive : archivesToLoad) {
            const std::string archiveKey = archive.lexically_normal().generic_string();
            if (loadedModArchives.contains(archiveKey)) {
                continue;
            }
            if (archiveManager->AddArchive(archiveKey) == nullptr) {
                error = "Failed to load mod archive: " + archive.string();
                return false;
            }
            loadedModArchives.insert(archiveKey);
        }

        if (isPak) {
            std::error_code filesystemError;
            if (!std::filesystem::remove(archivePath->second, filesystemError) && filesystemError) {
                error = "Converted archives were placed in the mods folder, but the original .pak could not be "
                        "removed: " +
                        filesystemError.message();
                return false;
            }
            removedConvertedPak = true;
            std::string cleanupError;
            if (!RemoveLegacyOotArchives(archivePath->second, cleanupError)) {
                SPDLOG_WARN("Could not remove obsolete OoT archives for {}: {}", archivePath->second.filename().string(),
                            cleanupError);
            }
            RemovePreviousPakConversionArchives(archivePath->second);
            SPDLOG_INFO("Removed converted source package {}", archivePath->second.string());
        }
    }

    if (removedConvertedPak) {
        UpdateModFiles(false, true);
    }

    if (convertedPak) {
        CVarSetInteger("gEnhancements.Mods.AlternateAssets", 1);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    archivesAdded = true;
    return true;
}

void ModMenuWindow::InitElement() {
    UpdateModFiles(true);
}

void RegisterModMenuWidgets() {
    enableModsWidget = { .name = "Enable Mods", .type = WidgetType::WIDGET_CVAR_CHECKBOX };
    enableModsWidget.CVar("gEnhancements.Mods.AlternateAssets")
        .Options(UIWidgets::CheckboxOptions({ { .disabledTooltip = "Temporarily disabled while editing mods list." } })
                     .Color(THEME_COLOR)
                     .Tooltip("Toggle mods. For graphics mods, this means toggling between default and mod graphics.")
                     .DefaultValue(false))
        .PreFunc([](WidgetInfo& info) {
            auto options = std::static_pointer_cast<UIWidgets::CheckboxOptions>(info.options);
            options->disabled = editing;
        });
    BenGui::mBenMenu->AddSearchWidget({ enableModsWidget, "Settings", "Mod Menu", "Top", "alternate assets" });

    tabHotkeyWidget = { .name = "Mods Tab Hotkey", .type = WidgetType::WIDGET_CVAR_CHECKBOX };
    tabHotkeyWidget.CVar("gEnhancements.Mods.AlternateAssetsHotkey")
        .Options(UIWidgets::CheckboxOptions()
                     .Color(THEME_COLOR)
                     .Tooltip("Allows pressing the Tab key to toggle mods")
                     .DefaultValue(true));
    BenGui::mBenMenu->AddSearchWidget(
        { tabHotkeyWidget, "Settings", "Mod Menu", "Top", "alternate assets tab hotkey" });
}

static RegisterMenuInitFunc menuInitFunc(RegisterModMenuWidgets);
