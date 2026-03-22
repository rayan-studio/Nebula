#include "core/window/Window.h"
#include "utils/logger/Logger.h"
#include <Windows.h>
#include <objbase.h>
#include <sstream>
#include "utils/crash/CrashHandler.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    CoInitialize(NULL);
    Utils::InitializeCrashHandler();
    Logger::Instance().Init(L"logs\\nebula.log");
    
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

#if defined(__MINGW32__)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR, int nCmdShow)
{
    return wWinMain(hInstance, hPrevInstance, GetCommandLineW(), nCmdShow);
}
#endif