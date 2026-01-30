#pragma once

#include <windows.h>
#include <string>

struct OpenFileRequest
{
    std::wstring filePath;
    int line = -1;
    int column = -1;
};

constexpr UINT WM_OPEN_FILE_AT = WM_USER + 101;
