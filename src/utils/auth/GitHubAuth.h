#pragma once

#include <string>

namespace GitHubAuth
{
    bool SaveToken(const std::wstring &token, std::wstring &outError);
    bool LoadToken(std::wstring &outToken, std::wstring &outError);
    bool HasToken();
    bool ClearToken(std::wstring &outError);
}

