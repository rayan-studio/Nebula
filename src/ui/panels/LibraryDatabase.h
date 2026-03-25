#pragma once
#include <string>
#include <vector>

// ============================================================================
// Library Database - Metadata for C++ libraries in Marketplace
// ============================================================================

struct LibraryInfo {
    std::wstring name;                    // e.g., "ImGui"
    std::wstring description;             // Short description
    std::wstring author;                  // Author or maintainer
    std::wstring gitUrl;                  // GitHub URL
    std::wstring category;                // "Rendering", "Graphics", "UI", etc
    std::wstring version;                 // Current version
    float rating = 4.5f;                  // Rating 0-5
    int downloads = 0;                    // Download count
    int stars = 0;                        // GitHub stars
    std::wstring icon;                    // Icon emoji or name
    std::vector<std::wstring> tags;       // Additional tags
    bool isInstalled = false;             // Installation status
    
    LibraryInfo() = default;
    
    LibraryInfo(
        const std::wstring& _name,
        const std::wstring& _desc,
        const std::wstring& _author,
        const std::wstring& _url,
        const std::wstring& _cat,
        const std::wstring& _ver,
        float _rating = 4.5f,
        int _downloads = 0,
        int _stars = 0,
        const std::wstring& _icon = L"📦")
        : name(_name)
        , description(_desc)
        , author(_author)
        , gitUrl(_url)
        , category(_cat)
        , version(_ver)
        , rating(_rating)
        , downloads(_downloads)
        , stars(_stars)
        , icon(_icon)
        , isInstalled(false) {}
};

// Library database manager (singleton)
class LibraryDatabase {
public:
    static LibraryDatabase& Instance();
    
    // Get all libraries
    const std::vector<LibraryInfo>& GetLibraries() const { return libraries_; }
    
    // Find library by name
    LibraryInfo* FindLibrary(const std::wstring& name);
    
    // Check if library is installed
    bool IsInstalled(const std::wstring& name) const;
    
    // Update installation status
    void SetInstalled(const std::wstring& name, bool installed);
    
    // Refresh installation status from filesystem.
    // If projectRoot is empty, falls back to detecting from executable location.
    void RefreshInstallationStatus(const std::wstring& projectRoot = L"");
    
private:
    LibraryDatabase();
    void InitializeDefaultLibraries();
    
    std::vector<LibraryInfo> libraries_;
};
