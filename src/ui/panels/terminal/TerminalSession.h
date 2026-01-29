#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <thread>

#include "ui/components/scrollbar/Scrollbar.h"

// Forward decl (vterm types cachés dans cpp)
struct VTerm;
struct VTermScreen;
struct VTermState;

class TerminalSession
{
public:
    TerminalSession();
    ~TerminalSession();

    // Init/shutdown
    bool Initialize(HWND hwndOwner, const std::wstring& startDir);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    // Input
    void OnChar(wchar_t ch);
    void OnKeyDown(WPARAM vk);
    void SendUtf8(const char* bytes, DWORD len);
    void SendText(const std::wstring& text);

    // PTY feed (appelé depuis Window via TerminalPanel::HandleConPTYOutput)
    void HandleConPTYOutput(const char* data, size_t len);

    // Layout & draw
    void SetViewport(float left, float top, float right, float bottom);
    void DrawContent(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                     const std::wstring& fontFamily, float fontSizePx, IDWriteFontCollection* fontCollection,
                     bool resizeHoverOrResizing, bool isFocused);

    // Scroll / mouse
    void OnMouseWheel(int wheelDelta);
    bool OnScrollbarMouseMove(POINT pt);
    bool OnScrollbarLButtonDown(POINT pt);
    bool OnScrollbarLButtonUp();
    bool OnLeftButtonDown(POINT pt);
    bool OnMouseMove(POINT pt, bool lmbDown);
    bool OnLeftButtonUp();

    float GetScrollOffset() const { return scrollbar_.GetScrollOffset(); }
    void SnapToBottomSoon() { pendingSnapToBottom_ = true; }

    // ConPTY resize (rows/cols) à partir des pixels
    void UpdatePseudoConsoleSizeFromPixels(float widthPx, float heightPx,
                                          const std::wstring& fontFamily, float fontSizePx, IDWriteFontCollection* fontCollection);

private:
    bool CopySelectionToClipboard();
    std::wstring BuildSelectionText() const;
    std::wstring BuildRowText(int row) const;

    void StartReadThread();
    void StopReadThread();
    void ReadLoop();

    // ConPTY dynamic loading (kernel32)
    bool LoadConPTYOnce();

    bool StartShellProcess(const std::wstring& startDir);
    void CloseConPTY();

    void WriteUtf8(const char* bytes, DWORD len);
    void WriteVtSequence(const char* seq);

    // VTerm
    void InitVTerm();
    void DestroyVTerm();
    void SetVTermSize(int rows, int cols);

private:
    // Owned by session
    bool initialized_ = false;
    bool conptyLoaded_ = false;

    HWND hwndOwner_ = NULL;

    HANDLE hInR_ = NULL;
    HANDLE hInW_ = NULL;
    HANDLE hOutR_ = NULL;
    HANDLE hOutW_ = NULL;
    HANDLE hChild_ = NULL;

    void* hPC_ = nullptr; // HPCON opaque

    // VTerm
    VTerm* vt_ = nullptr;
    VTermScreen* screen_ = nullptr;
    VTermState* state_ = nullptr;
    std::vector<std::wstring> scrollback_;
    int rows_ = 24;
    int cols_ = 80;

    // Rendering helpers
    IDWriteTextFormat* textFormat_ = nullptr; // cache par session (recréé si font change)
    std::wstring cachedFontFamily_;
    float cachedFontSize_ = 0.0f;
    IDWriteFontCollection* cachedFontCollection_ = nullptr; // non-owning

    float left_ = 0, top_ = 0, right_ = 0, bottom_ = 0;
    float charW_ = 8.0f;
    float lineH_ = 16.0f;
    float padX_ = 10.0f;
    float padY_ = 8.0f;

    // Scroll state
    bool userScrolling_ = false;
    bool pendingSnapToBottom_ = false;
    Scrollbar scrollbar_;

    bool selecting_ = false;
    bool hasSelection_ = false;
    int selectionStartRow_ = 0;
    int selectionStartCol_ = 0;
    int selectionEndRow_ = 0;
    int selectionEndCol_ = 0;
    bool suppressNextChar_ = false;

    std::atomic<bool> hasDamage_{ false };
    std::atomic<bool> stopReader_{ false };
    std::thread readerThread_;
};
