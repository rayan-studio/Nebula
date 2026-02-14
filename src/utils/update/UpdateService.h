#pragma once

#include <string>

namespace UpdateService
{
    enum class State
    {
        Idle,
        Checking,
        UpToDate,
        UpdateAvailable,
        Error
    };

    struct LatestInfo
    {
        std::wstring version;
        std::wstring noteVersion;
        std::wstring uploadDate;
        std::wstring portableUrl;
        std::wstring setupUrl;
    };

    // Triggers the first background check exactly once.
    void EnsureInitialized();

    // Triggers a new background check unless one is already in progress.
    void RefreshAsync();

    State GetState();
    bool HasUpdateAvailable();
    std::wstring GetStatusMessage();
    std::wstring GetCurrentVersion();
    LatestInfo GetLatestInfo();

    // Opens the preferred download URL (setup first, then portable).
    bool OpenPreferredDownload(std::wstring &outError);

    // Downloads setup, installs silently, then relaunches Nebula.
    // Caller should close the current process after this returns true.
    bool InstallUpdateAndRestart(std::wstring &outError);
}
