#include "path_helpers.h"
#include <windows.h>
#include <filesystem>

std::filesystem::path NebulaExeDir()
{
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

std::filesystem::path NebulaAssetPath(const std::filesystem::path& relative)
{
    std::filesystem::path exeDir = NebulaExeDir();
    std::filesystem::path direct = exeDir / L"assets" / relative;
    std::error_code ec;
    if (std::filesystem::exists(direct, ec))
        return direct;

    // Dev fallback: walk up a few parents to find /assets
    std::filesystem::path p = exeDir;
    for (int i = 0; i < 6 && !p.empty(); ++i)
    {
        std::filesystem::path cand = p / L"assets" / relative;
        if (std::filesystem::exists(cand, ec))
            return cand;
        p = p.parent_path();
    }

    return direct;
}
