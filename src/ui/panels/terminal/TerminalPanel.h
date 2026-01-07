#pragma once

#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <deque>

// Désactiver les macros Windows qui interfèrent avec libvterm
#ifdef MOUSE_MOVED
#undef MOUSE_MOVED
#endif
#ifdef small
#undef small
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// libvterm - DOIT être inclus APRÈS les headers Windows
#include "vterm.h"
// Structure pour une cellule de terminal
struct TerminalCell {
    wchar_t ch = L' ';
    COLORREF fgColor = 0xCCCCCC;
    COLORREF bgColor = 0x121212;
    bool bold = false;
    bool underline = false;
    bool inverse = false;
};

// Buffer de terminal - SIMPLIFIÉ car libvterm gère tout
class TerminalBuffer {
public:
    TerminalBuffer(int cols, int rows);
    
    void Resize(int cols, int rows);
    void Clear();
    
    void SetCell(int row, int col, const TerminalCell& cell);
    const TerminalCell& GetCell(int row, int col) const;
    
    int GetCols() const { return cols_; }
    int GetRows() const { return rows_; }
    
    // Dirty tracking pour rendering optimisé
    void MarkDirty(int row);
    void ClearDirtyFlag(int row);
    bool IsLineDirty(int row) const;
    bool HasDirtyLines() const;
    
    // Scrollback
    void AddToScrollback(const std::vector<TerminalCell>& line);
    const std::vector<TerminalCell>& GetScrollbackLine(int index) const;
    int GetScrollbackSize() const { return static_cast<int>(scrollback_.size()); }
    void ClearScrollback();

private:
    int cols_;
    int rows_;
    std::vector<std::vector<TerminalCell>> cells_;
    std::vector<bool> dirtyLines_;
    TerminalCell defaultCell_;
    
    static constexpr size_t MAX_SCROLLBACK = 10000;
    std::deque<std::vector<TerminalCell>> scrollback_;
};

// État du panel
struct TerminalPanelState {
    float leftEdge = 0;
    float rightEdge = 0;
    float topEdge = 0;
    float bottomEdge = 0;
    
    int logicalHeight = 300;
    int physicalHeight = 300;
    int minHeight = 100;
    int maxHeight = 800;
    
    float padding = 8.0f;
    float titleHeight = 28.0f;
    float charWidth = 9.0f;
    float lineHeight = 18.0f;
    
    bool isResizing = false;
    bool isHoveringResizeZone = false;
    int resizeStartY = 0;
    int resizeStartHeight = 0;
    
    int scrollOffset = 0;
};

// Sélection
struct TerminalSelection {
    int startRow = 0;
    int startCol = 0;
    int endRow = 0;
    int endCol = 0;
    bool selecting = false;
    bool active = false;
    
    bool HasSelection() const { return active || selecting; }
    void Clear() { active = false; selecting = false; }
};

// Panel principal
class TerminalPanel {
public:
    TerminalPanel();
    ~TerminalPanel();
    
    void Initialize();
    void Shutdown();
    
    void SetVisible(bool visible);
    bool IsVisible() const { return visible_; }
    void ToggleVisible();
    void Unfocus();
    bool IsFocused() const { return focused_; }
    bool IsInitialized() const { return vterm_ != nullptr; }
    
    void UpdateLayout(HWND hwnd, float editorLeft, float editorBottom, 
                     float editorRight, float footerTop);
    
    void Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd);
    
    // Mouse events
    bool OnMouseMove(HWND hwnd, POINT clientPoint);
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint);
    void OnLeftButtonUp(HWND hwnd);
    void OnRightButtonDown(HWND hwnd, POINT clientPoint);
    void OnMouseWheel(HWND hwnd, int delta);
    
    // Keyboard events
    void OnChar(wchar_t ch);
    void OnKeyDown(WPARAM key);
    
    // Hit testing
    bool IsPointInPanel(POINT pt) const;
    bool IsPointInResizeZone(POINT pt) const;
    bool IsResizing() const { return state_.isResizing; }
    
    const TerminalPanelState& GetState() const { return state_; }
    int GetPhysicalWidth() const { return 0; }
    
    // Actions
    void Copy();
    void Paste();
    void Clear();
    void SelectAll();
    void KillProcess();
    // Called from UI thread to handle ConPTY output posted from read thread
    void HandleConPTYOutput(char* buf, size_t len);
    void SetFocused(bool focused);

private:
    // libvterm - Remplace TOUT le parsing ANSI/VT100 !
    VTerm* vterm_ = nullptr;
    VTermScreen* vtscreen_ = nullptr;
    
    // Buffer
    std::unique_ptr<TerminalBuffer> buffer_;
    std::mutex bufferMutex_;
    int bufferCols_ = 80;
    int bufferRows_ = 24;
    
    // ConPTY
    HPCON hPC_ = nullptr;
    HANDLE hPipeIn_ = nullptr;
    HANDLE hPipeOut_ = nullptr;
    HANDLE hProcess_ = nullptr;
    HANDLE hThread_ = nullptr;
    bool processRunning_ = false;
    std::thread readThread_;
    std::wstring currentDirectory_;
    
    // État
    TerminalPanelState state_;
    bool visible_ = false;
    bool focused_ = false;
    
    // Curseur - libvterm nous donne la position
    int cursorRow_ = 0;
    int cursorCol_ = 0;
    bool cursorVisible_ = true;
    DWORD lastCursorBlink_ = 0;
    
    // Sélection
    TerminalSelection selection_;
    
    // Rendering
    bool forceFullRedraw_ = false;
    DWORD lastRenderTime_ = 0;
    static constexpr DWORD MIN_RENDER_INTERVAL_MS = 16; // 60 FPS
    static constexpr float RESIZE_ZONE_HEIGHT = 4.0f;
    
    // ConPTY
    bool StartConPTY();
    void StopConPTY();
    void ResizeConPTY();
    void WriteToConPTY(const char* data, size_t len);
    void WriteToConPTY(const std::wstring& text);
    void ReadThread();
    
    // libvterm processing - SIMPLIFIÉ !
    void ProcessOutput(const char* data, size_t len);
    void SyncCellsFromVTerm(); // Copie les cells de vterm vers notre buffer
    
    // libvterm callbacks
    static int OnDamage(VTermRect rect, void* user);
    static int OnMoveCursor(VTermPos pos, VTermPos oldpos, int visible, void* user);
    static int OnSetTermProp(VTermProp prop, VTermValue* val, void* user);
    
    // Drawing
    void DrawBackground(ID2D1RenderTarget* ctx, bool clearContentArea);
    void DrawBackground(ID2D1RenderTarget* ctx) { DrawBackground(ctx, false); }
    void DrawTopBorder(ID2D1RenderTarget* ctx);
    void DrawTitleBar(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite);
    void DrawContent(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, bool forceAll);
    void DrawCursor(ID2D1RenderTarget* ctx, float contentLeft, float contentTop);
    void DrawSelection(ID2D1RenderTarget* ctx, float contentLeft, float contentTop);
    
    // Hit testing helpers
    std::pair<int, int> PointToCell(POINT pt) const;
    bool HandleResizeMouseMove(HWND hwnd, POINT clientPoint);
    bool HandleResizeLeftButtonDown(HWND hwnd, POINT clientPoint);
    bool HandleResizeLeftButtonUp(HWND hwnd);
    
    // Selection
    void StartSelection(int row, int col);
    void UpdateSelection(int row, int col);
    void EndSelection();
    std::wstring GetSelectedText() const;
    
    // Conversion couleurs VTerm -> COLORREF
    static COLORREF VTermColorToColorRef(const VTermColor& color);
};

// Global accessor
TerminalPanel& GetTerminalPanel();