#pragma once
#include <Windows.h>
#include "orion/editor/Editor.h"
#include <string>
#include <map>
#include <memory>
#include "lsp/LspManager.h"
#include "ui/components/input/TextInput.h"

// Forward declare KeyMods used in message handling (defined in Window.cpp)
struct KeyMods;
#include "orion/editor/keyboard/KeyboardManager.h"
#include "ui/components/tabs/TabBar.h"
static constexpr UINT WM_EDITOR_FILE_LOADED = WM_USER + 777;
static constexpr UINT WM_EDITOR_FILE_RENAMED = WM_USER + 778;
static constexpr UINT WM_OPEN_SETTINGS = WM_USER + 779;
static constexpr UINT WM_LSP_DIAGNOSTICS = WM_USER + 780;
static constexpr UINT WM_OPEN_NEW_PROJECT = WM_USER + 781;

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

class Window
{
private:
    // Un éditeur par tab
    std::map<int, Orion::Editor *> editors_;
    TabBar tabBar_;
    int untitledCounter_ = 1;
    KeyboardManager keyboard_;
    std::unique_ptr<SettingsTabView> settingsTab_;
    std::map<int, int> pendingGoToLine_;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);

    bool UpdateTabBarHover(const POINT& ptClient);
    RECT GetTabBarRectClient() const;

    // Reset hover state for all top-level UI controls
    void ClearAllHoverStates();
    void RunActiveProject();
    void HandleCommandLineArgs();

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

    // Gestion multi-éditeurs
    Orion::Editor *GetEditorForTab(int tabIndex);
    void OpenFileInNewTab(const std::wstring &filePath, int lineNumber = -1);
    void OpenSettingsTab();
    bool IsSettingsTabIndex(int tabIndex) const;
    bool IsSettingsTabActive() const;
    static const std::wstring &SettingsTabPath();
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

private:
    enum class NewProjectPage
    {
        Home,
        Create,
    };
    // New Project overlay state
    bool newProjectVisible_ = false;
    NewProjectPage newProjPage_ = NewProjectPage::Home;
    TextInput newProjNameInput_;
    TextInput newProjLocationInput_;
    bool newProjNameFocused_ = true;
    bool newProjLocationFocused_ = false;
    bool newProjBrowseHover_ = false;
    bool newProjOpenHover_ = false;
    bool newProjCreateHover_ = false;
    bool newProjCancelHover_ = false;
    D2D1_RECT_F newProjCardRect_ = {};
    D2D1_RECT_F newProjBrowseRect_ = {};
    D2D1_RECT_F newProjOpenRect_ = {};
    D2D1_RECT_F newProjCreateRect_ = {};
    D2D1_RECT_F newProjCancelRect_ = {};
    std::vector<D2D1_RECT_F> newProjTemplateRects_;
    int newProjTemplateIndex_ = 0;
    int newProjTemplateHover_ = -1;
    bool CreateProjectFromOverlay();
};

// Fonctions globales pour récupérer Window depuis HWND
Window *GetWindowFromHwnd(HWND hwnd);
Orion::Editor *GetOrionEditor(HWND hwnd);
