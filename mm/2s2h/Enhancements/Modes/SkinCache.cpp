#include "SkinCache.h"
#include <ship/Context.h>
#include <ship/resource/archive/FolderArchive.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <stdexcept>
#include <algorithm>
#include <set>

namespace fs = std::filesystem;

static std::shared_ptr<Ship::Archive> sSkinCacheArchive = nullptr;
static std::string sSkinCacheDir;

namespace SkinCache {

void Unload() {
    if (sSkinCacheArchive != nullptr) {
        try {
            Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->RemoveArchive(sSkinCacheArchive);
        } catch (...) {
        }
        sSkinCacheArchive = nullptr;
    }
    // Clean up the cache directory
    if (!sSkinCacheDir.empty()) {
        std::error_code ec;
        fs::remove_all(sSkinCacheDir, ec);
        sSkinCacheDir.clear();
    }
}

std::shared_ptr<Ship::Archive> GetArchive() {
    return sSkinCacheArchive;
}

void Generate(bool isKafei) {
    try {
        // Unload any previous cache first
        Unload();

        // Create cache directory under the app directory (not the mods folder)
        auto appDir = Ship::Context::GetRawInstance()->GetAppDirectoryPath("2ship2harkinian");
        sSkinCacheDir = (fs::path(appDir) / "cache" / "skins" / (isKafei ? "kafei" : "vanilla")).string();
        std::error_code ec;
        fs::create_directories(sSkinCacheDir, ec);
        if (ec) {
            return;
        }

        // Determine the source object folder
        std::string srcFolder = isKafei ? "objects/object_test3" : "objects/object_link_child";

        // Extract ALL files from the source object folder in the base game OTR
        // and write them to the cache dir, preserving the folder structure.
        auto archiveManager = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager();
        if (!archiveManager) {
            return;
        }

        // Get all files from all archives to find those in the source folder
        auto archives = archiveManager->GetArchives();
        if (!archives) {
            return;
        }

        std::set<std::string> filesToExtract;
        for (auto& archive : *archives) {
            if (!archive) continue;
            auto fileList = archive->ListFiles();
            if (!fileList) continue;
            for (auto& [hash, path] : *fileList) {
                // Normalize path separators
                std::string normalizedPath = path;
                std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
                if (normalizedPath.find(srcFolder + "/") == 0) {
                    filesToExtract.insert(normalizedPath);
                }
            }
        }

        for (const auto& srcPath : filesToExtract) {
            auto file = archiveManager->LoadFile(srcPath);
            if (file && file->Buffer && !file->Buffer->empty()) {
                fs::path outPath = fs::path(sSkinCacheDir) / srcPath;
                std::error_code ec2;
                fs::create_directories(outPath.parent_path(), ec2);
                std::ofstream out(outPath, std::ios::binary);
                if (out.is_open()) {
                    out.write(file->Buffer->data(), file->Buffer->size());
                    out.close();
                }
            }
        }

        // Load the cache directory as a FolderArchive with max priority
        auto folderArchive = std::make_shared<Ship::FolderArchive>(sSkinCacheDir);
        if (folderArchive->Open()) {
            folderArchive->Load();
            sSkinCacheArchive = archiveManager->AddArchive(folderArchive);
            if (sSkinCacheArchive) {
                sSkinCacheArchive->SetPriority(INT32_MAX);
            }
        }
    } catch (...) {
        // Silently fail - don't crash the game
    }
}

} // namespace SkinCache