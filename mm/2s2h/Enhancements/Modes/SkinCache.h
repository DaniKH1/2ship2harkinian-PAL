#pragma once

#include <string>
#include <memory>

namespace Ship {
class Archive;
}

namespace SkinCache {
    // Generates a cached skin archive for the given skin (asynchronously).
    // isKafei: true for Kafei, false for vanilla Link.
    void Generate(bool isKafei);

    // Unloads any currently loaded skin cache archive.
    void Unload();

    // Returns the currently loaded skin cache archive (or nullptr).
    std::shared_ptr<Ship::Archive> GetArchive();
}