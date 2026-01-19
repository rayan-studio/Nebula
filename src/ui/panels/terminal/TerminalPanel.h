#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <atomic>

// Ton scrollbar (adapte include si besoin)
#include "ui/components/scrollbar/Scrollbar.h"

class TerminalPanel
{
public:
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
    bool IsInitialized() const { return initialized_; }

    const State& GetState() const { return state_; }

    // Init / shutdown
    bool Initialize(HWND hwnd, const std::wstring& startDir);
    void Shutdown();

    // Layout
    void UpdateLayout(HWND hwnd, float left, float top, float right, float bottom);
    bool IsPointInPanel(POINT pt) const;
    bool IsPointInResizeZone(POINT pt) const;

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

    // Font
    void SetFont(const std::wstring& family, float sizePx);
    void SetFontCollection(IDWriteFontCollection* fc);

    // ConPTY -> feed
    void HandleConPTYOutput(const char* data, size_t len);

private:
    // ConPTY
    bool StartShellProcess(const std::wstring& startDir);
    void StartReadThread(HWND hwnd);
    static DWORD WINAPI ReadThreadProc(LPVOID p);
    void CloseConPTY();

    // Sizing
    void UpdatePseudoConsoleSizeFromPixels();

    // VTerm (opaque, caché dans .cpp)
    void InitVTerm();
    void DestroyVTerm();
    void SetVTermSize(int rows, int cols);

    // Write to PTY
    void WriteUtf8(const char* bytes, DWORD len);
    void WriteVtSequence(const char* seq);

private:
    // Panel state
    bool visible_ = false;
    bool focused_ = false;

    bool resizing_ = false;
    bool resizeHover_ = false;

    float left_ = 0, top_ = 0, right_ = 0, bottom_ = 0;
    float resizeZoneH_ = 10.0f;
    float minHeight_ = 120.0f;

    POINT dragStart_{};
    float startTop_ = 0;

    State state_{};

    // Terminal dimensions
    int rows_ = 24;
    int cols_ = 80;

    // Fonts
    std::wstring fontFamily_ = L"JetBrains Mono";
    float fontSize_ = 13.0f;

    IDWriteTextFormat* textFormat_ = nullptr;
    IDWriteFontCollection* fontCollection_ = nullptr; // non-owning

    // Metrics
    float charW_ = 8.0f;
    float lineH_ = 16.0f;

    // Scrollback
    std::vector<std::wstring> scrollback_;
    bool userScrolling_ = false;
    bool pendingSnapToBottom_ = false;

    Scrollbar scrollbar_;

    // ConPTY handles
    bool initialized_ = false;
    bool conptyLoaded_ = false;

    HANDLE hInR_ = NULL;
    HANDLE hInW_ = NULL;
    HANDLE hOutR_ = NULL;
    HANDLE hOutW_ = NULL;
    HANDLE hChild_ = NULL;

    HANDLE hReadThread_ = NULL;
    std::atomic<bool> stopThread_{ false };

    HWND hwndOwner_ = NULL;

    // HPCON opaque
    void* hPC_ = nullptr;

    // VTerm opaque pointers (vrai type dans cpp)
    void* vt_ = nullptr;
    void* screen_ = nullptr;

    std::atomic<bool> hasDamage_{ false };
};

// Global accessor
TerminalPanel& GetTerminalPanel();
