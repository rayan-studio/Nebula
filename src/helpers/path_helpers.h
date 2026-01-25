#pragma once
#include <filesystem>

std::filesystem::path NebulaExeDir();
std::filesystem::path NebulaAssetPath(const std::filesystem::path& relative);
