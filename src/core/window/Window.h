#pragma once
#include <Windows.h>
#include "orion/editor/Editor.h"
#include <string>
#include <map>
#include <memory>
#include <vector>
#include <ctime>
#include <optional>
#include <unordered_set>
#include <mutex>
#include "lsp/LspManager.h"
#include "ui/components/input/TextInput.h"
#include "ui/screens/NewProjectOverlay.h"

// Forward declare KeyMods used in message handling (defined in Window.cpp)
struct KeyMods;
#include "orion/editor/keyboard/KeyboardManager.h"
#include "ui/components/tabs/TabBar.h"
static constexpr UINT WM_EDITOR_FILE_LOADED = WM_USER + 777;
static constexpr UINT WM_EDITOR_FILE_RENAMED = WM_USER + 778;
static constexpr UINT WM_OPEN_SETTINGS = WM_USER + 779;
static constexpr UINT WM_LSP_DIAGNOSTICS = WM_USER + 780;
static constexpr UINT WM_OPEN_NEW_PROJECT = WM_USER + 781;
static constexpr UINT WM_SHOW_RUN_ERROR_POPUP = WM_USER + 782;
static constexpr UINT WM_RUN_PROCESS_EXITED = WM_USER + 783;
static constexpr UINT WM_OPEN_MARKETPLACE_LIBRARY = WM_USER + 784;

struct RenamePathPayload
{
    std::wstring oldPath;
    std::wstring newPath;
};

struct LspDiagnosticsResult
{
    int tabIndex = -1;
    std::wstring filePath;
    std::vector<Lsp::Diagnostic> diagnostics;
};

class Skia;
class SettingsTabView;
class MarketplaceExtensionTabView;

class Window
{
private:
    friend class UI::NewProjectOverlay;
    friend class UI::NewProjectHomeView;
    friend class UI::NewProjectCreateView;
    // Un éditeur par tab
    std::map<int, Orion::Editor *> editors_;
    TabBar tabBar_;
    int untitledCounter_ = 1;
    KeyboardManager keyboard_;
    std::unique_ptr<SettingsTabView> settingsTab_;
    std::unique_ptr<MarketplaceExtensionTabView> marketplaceTab_;
    std::map<int, std::pair<int, int>> pendingGoToLocation_;
    std::unordered_set<int> pendingMarkdownPreview_;
    std::optional<Lsp::Location> pendingContextGoto_;
    mutable std::mutex runProcessMutex_;
    HANDLE runProcessHandle_ = nullptr;
    HANDLE runJobHandle_ = nullptr;
    DWORD runProcessId_ = 0;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);

    bool UpdateTabBarHover(const POINT& ptClient);
    RECT GetTabBarRectClient() const;

    // Reset hover state for all top-level UI controls
    void ClearAllHoverStates();
    void RunActiveProject();
    void StopActiveRunProcess();
    bool IsRunProcessActive() const;
    bool TrackRunProcess(HANDLE processHandle, HANDLE threadHandle, DWORD processId);
    void ClearTrackedRunProcess();
    bool HandleCommandLineArgs();
    void StartTitlebarHoverAnimation();
    void StepTitlebarHoverAnimation();
    void ScheduleDiagnosticsForTab(int tabIndex);

    HINSTANCE hInstance_;
    HWND hwnd_;
    Skia *skia_;
    std::wstring text_;
    std::wstring customFontPath_;
    IDWriteFontCollection *uiFontCollection_ = nullptr;

public:
    std::wstring GetCustomFontPath() const { return customFontPath_; }
    Window(HINSTANCE hInstance);
    ~Window();
    struct EditorFileLoadResult
    {
        int tabIndex = -1;
        std::wstring filePath;
        std::wstring encoding;
        std::vector<std::wstring> lines;
        bool isPreview = false;
        HBITMAP previewBitmap = nullptr;
        SIZE previewSize = {0, 0};
        std::wstring previewMessage;
    };

    bool Create(int nCmdShow);
    int Run();
    void SetText(const std::wstring &text);

    Orion::Editor *GetEditor();
    TabBar *GetTabBar() { return &tabBar_; }
    SettingsTabView *GetSettingsTabView() { return settingsTab_.get(); }
    MarketplaceExtensionTabView *GetMarketplaceExtensionTabView() { return marketplaceTab_.get(); }
    void SetUpdateToastRect(const D2D1_RECT_F &rect);
    void ClearUpdateToastRect();
    bool IsPointInUpdateToast(POINT pt) const;
    bool SetUpdateToastHovered(bool hovered);
    bool IsUpdateToastHovered() const { return updateToastHovered_; }
    void SetUpdateDismissRect(const D2D1_RECT_F &rect);
    bool IsPointInUpdateDismiss(POINT pt) const;
    bool SetUpdateDismissHovered(bool hovered);
    bool IsUpdateDismissHovered() const { return updateDismissHovered_; }
    void SetGitHubBadgeRect(const D2D1_RECT_F &rect);
    void ClearGitHubBadgeRect();
    bool IsPointInGitHubBadge(POINT pt) const;
    bool SetGitHubBadgeHovered(bool hovered);
    bool IsGitHubBadgeHovered() const { return githubBadgeHovered_; }
    void DismissUpdateToast() { updateToastDismissed_ = true; }
    bool IsUpdateToastDismissed() const { return updateToastDismissed_; }
    void ResetUpdateToastDismissed() { updateToastDismissed_ = false; }

    // Gestion multi-éditeurs
    Orion::Editor *GetEditorForTab(int tabIndex);
    void OpenFileInNewTab(const std::wstring &filePath, int lineNumber = -1, int column = -1);
    void OpenFileInNewTabWithMarkdownPreview(const std::wstring &filePath);
    void OpenMarketplaceLibraryTab(const std::wstring &libraryName);
    void OpenSettingsTab();
    bool IsSettingsTabIndex(int tabIndex) const;
    bool IsMarketplaceTabIndex(int tabIndex) const;
    bool IsSettingsTabActive() const;
    static const std::wstring &SettingsTabPath();
    static const std::wstring &MarketplaceTabPrefix();
    static std::wstring MarketplaceLibraryNameFromTabPath(const std::wstring &tabPath);
    // Dialogs
    void OpenFileDialog();
    void OpenProjectDialog();
    bool CreateCppConsoleProject(const std::wstring &rootPath, const std::wstring &projectName, std::wstring &outMainFile);
    void ShowNewProjectOverlay();
    void HideNewProjectOverlay();
    bool IsNewProjectOverlayVisible() const { return newProjectVisible_; }
    void DrawNewProjectOverlay(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, const RECT &clientRect);
    bool HandleNewProjectMouseDown(HWND hwnd, POINT pt);
    bool HandleNewProjectMouseUp(HWND hwnd, POINT pt);
    bool HandleNewProjectMouseMove(HWND hwnd, POINT pt);
    bool HandleNewProjectChar(wchar_t ch);
    bool HandleNewProjectKeyDown(WPARAM key);

    // Global shortcut handler
    bool HandleGlobalShortcuts(WPARAM wParam, const struct KeyMods &m);

    // Accessors used by other components
    HWND GetHwnd() const;
    void CloseEditorForTabIndex(int index);

    enum CustomTitleBarHoveredButton
    {
        Hovered_None = 0,
        Hovered_Run,
        Hovered_Minimize,
        Hovered_Maximize,
        Hovered_Close,
    };
    CustomTitleBarHoveredButton hoveredButton_ = Hovered_None;
    float titlebarHoverMin_ = 0.0f;
    float titlebarHoverMax_ = 0.0f;
    float titlebarHoverClose_ = 0.0f;
    float titlebarHoverRun_ = 0.0f;
    bool titlebarHoverAnimating_ = false;
    DWORD titlebarHoverLastTick_ = 0;
    std::map<int, DWORD> pendingDiagTick_;
    bool diagTimerActive_ = false;
    DWORD lastExternalFileCheckTick_ = 0;
    int lastUpdateStateSnapshot_ = -1;
    std::wstring lastUpdateStatusMessage_;
    D2D1_RECT_F updateToastRect_ = D2D1::RectF(0, 0, 0, 0);
    bool updateToastHovered_ = false;
    D2D1_RECT_F updateDismissRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F githubBadgeRect_ = D2D1::RectF(0, 0, 0, 0);
    bool githubBadgeHovered_ = false;
    bool updateDismissHovered_ = false;
    bool updateToastDismissed_ = false;

public:
    float GetTitlebarHoverAlpha(CustomTitleBarHoveredButton btn) const;

private:
    enum class NewProjectPage
    {
        Home,
        Create,
    };
    // New Project overlay state
    bool newProjectVisible_ = false;
    NewProjectPage newProjPage_ = NewProjectPage::Home;
    UI::NewProjectOverlay newProjOverlay_;
    UI::NewProjectHomeView newProjHomeView_;
    UI::NewProjectCreateView newProjCreateView_;
    TextInput newProjNameInput_;
    TextInput newProjLocationInput_;
    bool newProjNameFocused_ = true;
    bool newProjLocationFocused_ = false;
    bool newProjBrowseHover_ = false;
    bool newProjOpenHover_ = false;
    bool newProjCreateHover_ = false;
    bool newProjCancelHover_ = false;
    bool skipNewProjectOverlayOnce_ = false;
    D2D1_RECT_F newProjCardRect_ = {};
    D2D1_RECT_F newProjBrowseRect_ = {};
    D2D1_RECT_F newProjOpenRect_ = {};
    D2D1_RECT_F newProjCreateRect_ = {};
    D2D1_RECT_F newProjCancelRect_ = {};
    std::vector<D2D1_RECT_F> newProjTemplateRects_;
    int newProjTemplateIndex_ = 0;
    int newProjTemplateHover_ = -1;
    struct RecentProjectEntry
    {
        std::wstring path;
        std::time_t lastOpened = 0;
    };
    std::vector<RecentProjectEntry> recentProjects_;
    std::vector<D2D1_RECT_F> recentProjectRects_;
    std::vector<int> recentProjectIndexMap_;
    int recentProjectHover_ = -1;
    bool CreateProjectFromOverlay();
    void LoadRecentProjects();
    void SaveRecentProjects() const;
    void AddRecentProject(const std::wstring &path);
    void OpenProjectAtPath(const std::wstring &path);
};

// Fonctions globales pour récupérer Window depuis HWND
Window *GetWindowFromHwnd(HWND hwnd);

// Tampon (template) applied to new files
const std::wstring &GetTamponText();
bool HasTamponText();
void SetTamponText(const std::wstring &text);
void ClearTamponText();
Orion::Editor *GetOrionEditor(HWND hwnd);
