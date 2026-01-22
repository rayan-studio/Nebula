#pragma once
#include <Windows.h>
#include "orion/editor/Editor.h"
#include <string>
#include <map>

// Forward declare KeyMods used in message handling (defined in Window.cpp)
struct KeyMods;
#include "orion/editor/keyboard/KeyboardManager.h"
#include "ui/components/tabs/TabBar.h"
static constexpr UINT WM_EDITOR_FILE_LOADED = WM_USER + 777;

class Skia;

class Window
{
private:
    // Un éditeur par tab
    std::map<int, Orion::Editor *> editors_;
    TabBar tabBar_;
    int untitledCounter_ = 1;
    KeyboardManager keyboard_;

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

    // Gestion multi-éditeurs
    Orion::Editor *GetEditorForTab(int tabIndex);
    void OpenFileInNewTab(const std::wstring &filePath, int lineNumber = -1);
    // Dialogs
    void OpenFileDialog();
    void OpenProjectDialog();

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
};

// Fonctions globales pour récupérer Window depuis HWND
Window *GetWindowFromHwnd(HWND hwnd);
Orion::Editor *GetOrionEditor(HWND hwnd);
