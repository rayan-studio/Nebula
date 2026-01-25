#include "path_helpers.h"
#include <windows.h>

std::filesystem::path NebulaExeDir()
{
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

std::filesystem::path NebulaAssetPath(const std::filesystem::path& relative)
{
    return NebulaExeDir() / L"assets" / relative;
}
