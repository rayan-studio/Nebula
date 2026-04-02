#pragma once

#include <windows.h>

#include <string>
#include <vector>
#include <mutex>
#include <atomic>

struct LibraryInfo
{
    enum class InstallState
    {
        Idle,
        Installing,
        Error,
    };

    int id = 0;
    std::wstring name;
    std::wstring description;
    std::wstring readmeSummary;
    std::wstring author;
    std::wstring gitUrl;
    std::wstring category;
    std::wstring version;
    std::wstring language;
    std::wstring license;
    std::wstring homepage;
    std::wstring status;
    std::wstring avatarUrl;
    float rating = 0.0f;
    int downloads = 0;
    int stars = 0;
    std::vector<std::wstring> tags;
    bool isInstalled = false;
    InstallState installState = InstallState::Idle;
    std::wstring installMessage;
};

class LibraryDatabase
{
public:
    static LibraryDatabase& Instance();

    std::vector<LibraryInfo> GetLibrariesCopy() const;
    bool GetLibraryCopy(const std::wstring& name, LibraryInfo& outLibrary) const;

    bool IsInstalled(const std::wstring& name) const;
    void SetInstalled(const std::wstring& name, bool installed);
    void SetInstallStatus(const std::wstring& name,
                          LibraryInfo::InstallState state,
                          const std::wstring& message = L"");
    void RefreshInstallationStatus(const std::wstring& projectRoot = L"");

    void RequestLibrariesAsync(const std::wstring& searchQuery, HWND hwnd);
    bool IsLoading() const;
    std::wstring GetLastError() const;
    unsigned long long GetRevision() const { return revision_.load(); }

private:
    LibraryDatabase();
    void BumpRevision();

    mutable std::mutex mutex_;
    std::vector<LibraryInfo> libraries_;
    std::wstring lastProjectRoot_;
    std::wstring lastRequestedQuery_;
    std::wstring lastError_;
    bool isLoading_ = false;
    bool hasLoadedOnce_ = false;
    std::atomic<unsigned long long> revision_{0};
    std::atomic<unsigned long long> nextRequestId_{0};
};
