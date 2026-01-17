#pragma once
#include <Windows.h>
#include "orion/editor/Editor.h"
#include "ui/components/tabs/TabBar.h"
#include <string>
#include <map>

class Skia;

class Window
{
public:
    Window(HINSTANCE hInstance);
    ~Window();

    bool Create(int nCmdShow);
    int Run();
    void SetText(const std::wstring &text);

    Orion::Editor *GetEditor();
    TabBar *GetTabBar() { return &tabBar_; }
    
    // Gestion multi-éditeurs
    Orion::Editor *GetEditorForTab(int tabIndex);
    void OpenFileInNewTab(const std::wstring& filePath, int lineNumber = -1);

private:
    // Un éditeur par tab
    std::map<int, Orion::Editor*> editors_;
    TabBar tabBar_;
    int untitledCounter_ = 1;
    
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);

    // Reset hover state for all top-level UI controls
    void ClearAllHoverStates();

    HINSTANCE hInstance_;
    HWND hwnd_;
    Skia *skia_;
    std::wstring text_;
    std::wstring customFontPath_;
public:
    std::wstring GetCustomFontPath() const { return customFontPath_; }
    
    enum CustomTitleBarHoveredButton
    {
        Hovered_None = 0,
        Hovered_Minimize,
        Hovered_Maximize,
        Hovered_Close,
    };
    CustomTitleBarHoveredButton hoveredButton_ = Hovered_None;
};

// Fonctions globales pour récupérer Window depuis HWND
Window* GetWindowFromHwnd(HWND hwnd);
Orion::Editor* GetOrionEditor(HWND hwnd);