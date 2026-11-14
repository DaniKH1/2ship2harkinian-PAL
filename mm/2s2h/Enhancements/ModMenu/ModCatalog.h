#pragma once

#include <filesystem>

namespace ModCatalog {
bool Draw(const std::filesystem::path& modsDirectory, bool gameStarted);
void Shutdown(const std::filesystem::path& modsDirectory);
}
