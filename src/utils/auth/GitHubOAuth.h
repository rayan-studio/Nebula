#pragma once

#include <string>

namespace GitHubOAuth
{
    struct AuthResult
    {
        bool success = false;
        std::wstring message;
    };

    AuthResult SignInViaBrowser();
}

