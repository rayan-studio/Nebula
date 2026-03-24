#include "LibraryDatabase.h"
#include <filesystem>
#include <algorithm>
#include <windows.h>

namespace fs = std::filesystem;

LibraryDatabase& LibraryDatabase::Instance()
{
    static LibraryDatabase instance;
    return instance;
}

LibraryDatabase::LibraryDatabase()
{
    InitializeDefaultLibraries();
    RefreshInstallationStatus();
}

void LibraryDatabase::InitializeDefaultLibraries()
{
    libraries_.clear();
    
    libraries_.push_back(LibraryInfo(
        L"ImGui",
        L"Immediate Mode GUI library for C++",
        L"ocornut",
        L"https://github.com/ocornut/imgui.git",
        L"UI",
        L"1.89",
        4.8f, 28500, 12800, L"🎨"
    ));
    
    libraries_.push_back(LibraryInfo(
        L"GLAD",
        L"Multi-language OpenGL Loader Generator",
        L"Dav1dde",
        L"https://github.com/Dav1dde/glad.git",
        L"Graphics",
        L"2.0",
        4.5f, 5200, 3200, L"⚡"
    ));
    
    libraries_.push_back(LibraryInfo(
        L"GLFW",
        L"Window creation and input handling library",
        L"glfw",
        L"https://github.com/glfw/glfw.git",
        L"Graphics",
        L"3.3.8",
        4.6f, 8900, 6500, L"🪟"
    ));
    
    libraries_.push_back(LibraryInfo(
        L"nlohmann/json",
        L"JSON for Modern C++ library",
        L"nlohmann",
        L"https://github.com/nlohmann/json.git",
        L"Utilities",
        L"3.11.2",
        4.7f, 42000, 18900, L"📋"
    ));
    
    libraries_.push_back(LibraryInfo(
        L"glm",
        L"OpenGL Mathematics library",
        L"g-truc",
        L"https://github.com/g-truc/glm.git",
        L"Math",
        L"0.9.9",
        4.4f, 15600, 7200, L"✖️"
    ));

    libraries_.push_back(LibraryInfo(
        L"ggwave",
        L"Data-over-sound library for C/C++",
        L"ggerganov",
        L"https://github.com/ggerganov/ggwave.git",
        L"Audio",
        L"0.4.2",
        4.3f, 1800, 1400, L"🔊"
    ));

    libraries_.push_back(LibraryInfo(
        L"nanosvg",
        L"Simple SVG parser and rasterizer",
        L"memononen",
        L"https://github.com/memononen/nanosvg.git",
        L"Graphics",
        L"1.0",
        4.2f, 2100, 1900, L"🖼"
    ));

    libraries_.push_back(LibraryInfo(
        L"libgit2",
        L"Cross-platform linkable library for Git",
        L"libgit2",
        L"https://github.com/libgit2/libgit2.git",
        L"Utilities",
        L"1.7.1",
        4.6f, 9800, 5400, L"🔗"
    ));

    libraries_.push_back(LibraryInfo(
        L"libvterm",
        L"VT220/xterm-compatible terminal emulator library",
        L"neovim",
        L"https://github.com/neovim/libvterm.git",
        L"Utilities",
        L"0.3",
        4.0f, 800, 600, L"🖥"
    ));

    libraries_.push_back(LibraryInfo(
        L"fmt",
        L"Fast and safe formatting library for C++",
        L"fmtlib",
        L"https://github.com/fmtlib/fmt.git",
        L"Utilities",
        L"10.1.1",
        4.9f, 38000, 17500, L"📝"
    ));

    libraries_.push_back(LibraryInfo(
        L"spdlog",
        L"Fast C++ logging library",
        L"gabime",
        L"https://github.com/gabime/spdlog.git",
        L"Utilities",
        L"1.12.0",
        4.8f, 22000, 19000, L"📋"
    ));

    libraries_.push_back(LibraryInfo(
        L"stb",
        L"Single-file public domain libraries for C/C++",
        L"nothings",
        L"https://github.com/nothings/stb.git",
        L"Utilities",
        L"master",
        4.7f, 31000, 24000, L"📦"
    ));

    libraries_.push_back(LibraryInfo(
        L"Catch2",
        L"Modern C++ test framework",
        L"catchorg",
        L"https://github.com/catchorg/Catch2.git",
        L"Testing",
        L"3.4.0",
        4.7f, 14000, 17000, L"🧪"
    ));

    libraries_.push_back(LibraryInfo(
        L"asio",
        L"Asynchronous I/O and networking for C++",
        L"chriskohlhoff",
        L"https://github.com/chriskohlhoff/asio.git",
        L"Networking",
        L"1.28.0",
        4.5f, 11000, 4400, L"🌐"
    ));

    libraries_.push_back(LibraryInfo(
        L"box2d",
        L"2D rigid body physics engine for games",
        L"erincatto",
        L"https://github.com/erincatto/box2d.git",
        L"Physics",
        L"2.4.1",
        4.6f, 9200, 6800, L"⚙"
    ));
}

LibraryInfo* LibraryDatabase::FindLibrary(const std::wstring& name)
{
    auto it = std::find_if(libraries_.begin(), libraries_.end(),
        [&name](const LibraryInfo& lib) {
            return _wcsicmp(lib.name.c_str(), name.c_str()) == 0;
        });
    
    if (it != libraries_.end())
        return &(*it);
    return nullptr;
}

bool LibraryDatabase::IsInstalled(const std::wstring& name) const
{
    auto it = std::find_if(libraries_.begin(), libraries_.end(),
        [&name](const LibraryInfo& lib) {
            return _wcsicmp(lib.name.c_str(), name.c_str()) == 0;
        });
    
    return it != libraries_.end() && it->isInstalled;
}

void LibraryDatabase::SetInstalled(const std::wstring& name, bool installed)
{
    auto lib = FindLibrary(name);
    if (lib)
        lib->isInstalled = installed;
}

void LibraryDatabase::RefreshInstallationStatus()
{
    // Walk up from the exe directory to find the project root (contains CMakeLists.txt)
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    fs::path dir = fs::path(exePath).parent_path();
    fs::path externalDir;

    for (int i = 0; i < 6; ++i)
    {
        if (fs::exists(dir / L"CMakeLists.txt"))
        {
            externalDir = dir / L"external";
            break;
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) break;  // filesystem root
        dir = parent;
    }

    if (externalDir.empty())
        return; // Cannot determine project root — leave all as not installed

    for (auto& lib : libraries_)
    {
        fs::path libPath = externalDir / lib.name;
        lib.isInstalled = fs::exists(libPath) && fs::is_directory(libPath);
    }
}
