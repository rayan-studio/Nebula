#pragma once

#include <Windows.h>

namespace Utils
{
    // Initialize crash handler (creates minidump on unhandled exceptions)
    void InitializeCrashHandler();
}
