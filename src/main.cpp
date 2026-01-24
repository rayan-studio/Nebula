#include "core/window/Window.h"
#include "utils/logger/Logger.h"
#include <Windows.h>
#include <objbase.h>
#include <sstream>
// Crash handler to produce minidumps for native crashes
#include "utils/crash/CrashHandler.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    CoInitialize(NULL);
    // initialize crash handler early so native crashes produce a minidump
    Utils::InitializeCrashHandler();
    // initialize logger (logs/nebula.log relative to working directory)
    Logger::Instance().Init(L"logs\\nebula.log");
    // Try to register bundled JetBrains Mono fonts privately for this process
    {
        wchar_t modulePath[MAX_PATH] = {0};
        if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) > 0)
        {
            std::wstring dir(modulePath);
            size_t pos = dir.find_last_of(L"\\/");
            if (pos != std::wstring::npos)
                dir = dir.substr(0, pos);
            // Try several candidate locations relative to the executable dir:
            // 1) <exe_dir>\assets\font\static
            // 2) <exe_dir>\..\assets\font\static
            // 3) <exe_dir>\..\..\assets\font\static
            std::vector<std::wstring> candidates;
            candidates.push_back(dir + L"\\assets\\font\\static\\JetBrainsMono-Regular.ttf");
            candidates.push_back(dir + L"\\..\\assets\\font\\static\\JetBrainsMono-Regular.ttf");
            candidates.push_back(dir + L"\\..\\..\\assets\\font\\static\\JetBrainsMono-Regular.ttf");                                            

            std::wstring foundPath;
            for (const auto &cand : candidates)
            {
                wchar_t full[MAX_PATH] = {0};
                if (GetFullPathNameW(cand.c_str(), MAX_PATH, full, NULL) > 0)
                {
                    DWORD attr2 = GetFileAttributesW(full);
                    if (attr2 != INVALID_FILE_ATTRIBUTES)
                    {
                        foundPath = full;
                        break;
                    }
                }
            }

            if (!foundPath.empty())
            {
                int added = AddFontResourceExW(foundPath.c_str(), FR_PRIVATE, 0);
                std::wstringstream ss;
                ss << L"AddFontResourceExW(" << foundPath << L") returned=" << added;
                Logger::Instance().Log(ss.str());
            }
            else
            {
                std::wstringstream ss;
                ss << L"Bundled font not found in candidates; looked relative to: " << dir;
                Logger::Instance().Log(ss.str());
            }
        }
    }

    Window win(hInstance);
    if (!win.Create(nCmdShow)) {
        CoUninitialize();
        return -1;
    }

    int res = win.Run();

    // close logger
    Logger::Instance().Close();
    CoUninitialize();
    return res;
}