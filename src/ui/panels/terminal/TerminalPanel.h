#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

// Scrollbar (toujours dispo si besoin ailleurs)
#include "ui/components/scrollbar/Scrollbar.h"
#include "ui/components/tabs/TabBar.h"

class TerminalSession;

class TerminalPanel
{
public:
    struct ProblemItem
    {
        std::wstring fileName;
        int line = 0;
        int column = 0;
        bool isError = false;
        std::wstring message;
    };

    struct State
    {
        float leftEdge = 0;
        float topEdge = 0;
        float rightEdge = 0;
        float bottomEdge = 0;
        int physicalWidth = 0;
    };

public:
    TerminalPanel();
    ~TerminalPanel();

    // Visible / focus
    void ToggleVisible();
    void SetVisible(bool v);
    bool IsVisible() const { return visible_; }

    void Unfocus();
    void SetFocused(bool f) { focused_ = f; }
    bool IsFocused() const { return focused_; }

    bool IsResizing() const { return resizing_; }
    bool IsInitialized() const; // true si au moins une session initialisée

    const State& GetState() const { return state_; }
    float GetHeightPx() const { return heightPx_; }
    void SetHeightPx(float h) { heightPx_ = h; }

    // Multi-terminal API
    int  GetTerminalCount() const;
    int  GetActiveIndex() const { return activeIndex_; }
    void SetActiveIndex(int idx);
    void NewTerminal(HWND hwnd, const std::wstring& startDir);
    void CloseTerminal(int idx);
    void CloseAll();

    // Public helpers to ensure sessions (wrap private helpers)
    void EnsureSessionExists(HWND hwnd);
    void EnsureActiveInit(HWND hwnd);

    // Layout
    void UpdateLayout(HWND hwnd, float left, float top, float right, float bottom);
    bool IsPointInPanel(POINT pt) const;
    bool IsPointInResizeZone(POINT pt) const;
    bool IsPointInTabsBarArea(POINT pt) const;
    bool IsPointInPlusButton(POINT pt) const;
    bool IsShowingOutputOrProblems() const { return showOutput_ || showProblems_; }

    // Mouse
    void OnLeftButtonDown(HWND hwnd, POINT pt);
    void OnLeftButtonUp(HWND hwnd);
    bool OnMouseMove(HWND hwnd, POINT pt);
    void OnMouseWheel(HWND hwnd, int wheelDelta);

    // Keyboard
    void OnChar(wchar_t ch);
    void OnKeyDown(WPARAM vk);

    // Render
    void Draw(ID2D1RenderTarget* rt, IDWriteFactory* dwrite);
    void Draw(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, HWND hwnd);
    void SetProblems(const std::wstring& filePath, const std::vector<ProblemItem>& problems);
    void ClearOutput();
    void AppendOutputChunk(const std::wstring& text);
    void FlushOutputBuffer();
    void ShowOutput(bool v);
    bool IsOutputVisible() const { return showOutput_; }
    size_t GetOutputLineCount() const { return outputLines_.size(); }

    // Run a command in the active terminal session (Nebula Dev Shell).
    bool SendCommandToActive(HWND hwnd, const std::wstring& startDir, const std::wstring& command);

    // Font
    void SetFont(const std::wstring& family, float sizePx);
    void SetFontCollection(IDWriteFontCollection* fc);

    // ConPTY -> feed
    void HandleConPTYOutput(const char* data, size_t len);

private:
    // UI helpers
    float TabsBarHeightPx() const { return tabBar_.GetHeight(); }

    RECT  TabsBarRectClient() const;
    RECT  PlusButtonRectClient() const;
    RECT  ProblemsButtonRectClient() const;
    RECT  OutputButtonRectClient() const;
    bool  HitTestPlus(POINT pt) const;
    bool  HitTestProblems(POINT pt) const;
    bool  HitTestOutput(POINT pt) const;
    bool  IsPointInTabsBar(POINT pt) const;
    float TabsBarRightEdge() const;

    void SyncTabBar();

    // Active session helpers
    TerminalSession* ActiveSession();
    const TerminalSession* ActiveSession() const;

    void EnsureAtLeastOneSession(HWND hwnd);
    void EnsureActiveInitialized(HWND hwnd);

    // Sizing
    void UpdatePseudoConsoleSizeFromPixelsForActive();

private:
    int AllocateSessionId();

    // Panel state
    bool visible_ = false;
    bool focused_ = false;

    bool resizing_ = false;
    bool resizeHover_ = false;

    float left_ = 0, top_ = 0, right_ = 0, bottom_ = 0;
    float resizeZoneH_ = 6.0f;
    float minHeight_ = 120.0f;
    float heightPx_ = 0.0f;

    POINT dragStart_{};
    float startTop_ = 0;

    State state_{};

    // Fonts
    std::wstring fontFamily_ = L"JetBrains Mono";
    float fontSize_ = 13.0f;
    IDWriteFontCollection* fontCollection_ = nullptr; // non-owning

    // Multi sessions
    std::vector<std::unique_ptr<TerminalSession>> sessions_;
    int activeIndex_ = -1;
    int nextSessionId_ = 1;
    std::vector<int> sessionIds_;

    // Tabs hover
    bool hoveredPlus_ = false;
    bool hoveredProblems_ = false;
    bool hoveredOutput_ = false;
    bool showProblems_ = false;
    bool showOutput_ = false;

    TabBar tabBar_;
    std::vector<ProblemItem> problems_;
    std::wstring problemsFilePath_;

    std::vector<std::wstring> outputLines_;
    std::wstring outputBuffer_;
    std::mutex outputMutex_;
};

// Global accessor
TerminalPanel& GetTerminalPanel();
