#include "TerminalPanel.h"
#include <algorithm>
#include "utils/logger/Logger.h"

// ... reste du code
static TerminalPanel g_terminalPanel;
TerminalPanel& GetTerminalPanel() { return g_terminalPanel; }

// Disable terminal panel entirely (no-op stub)
static constexpr bool TERMINAL_PANEL_DISABLED = true;

// ============================================================================
// TerminalBuffer Implementation (simplifié)
// ============================================================================

TerminalBuffer::TerminalBuffer(int cols, int rows) : cols_(cols), rows_(rows) {
    defaultCell_.ch = L' ';
    defaultCell_.fgColor = 0xCCCCCC;
    defaultCell_.bgColor = 0x121212;
    cells_.resize(rows, std::vector<TerminalCell>(cols, defaultCell_));
    dirtyLines_.resize(rows, true);
}

void TerminalBuffer::Resize(int cols, int rows) {
    if (cols == cols_ && rows == rows_) return;
    
    std::vector<std::vector<TerminalCell>> newCells(rows, std::vector<TerminalCell>(cols, defaultCell_));
    
    int copyRows = std::min(rows, rows_);
    int copyCols = std::min(cols, cols_);
    
    for (int r = 0; r < copyRows; ++r) {
        for (int c = 0; c < copyCols; ++c) {
            newCells[r][c] = cells_[r][c];
        }
    }
    
    cells_ = std::move(newCells);
    dirtyLines_.resize(rows, true);
    cols_ = cols;
    rows_ = rows;
}

void TerminalBuffer::Clear() {
    for (int r = 0; r < rows_; ++r) {
        std::fill(cells_[r].begin(), cells_[r].end(), defaultCell_);
        MarkDirty(r);
    }
}

void TerminalBuffer::SetCell(int row, int col, const TerminalCell& cell) {
    if (row >= 0 && row < rows_ && col >= 0 && col < cols_) {
        cells_[row][col] = cell;
        MarkDirty(row);
    }
}

const TerminalCell& TerminalBuffer::GetCell(int row, int col) const {
    static TerminalCell dummy;
    if (row >= 0 && row < rows_ && col >= 0 && col < cols_) {
        return cells_[row][col];
    }
    return dummy;
}

void TerminalBuffer::MarkDirty(int row) {
    if (row >= 0 && row < rows_) {
        dirtyLines_[row] = true;
    }
}

void TerminalBuffer::ClearDirtyFlag(int row) {
    if (row >= 0 && row < rows_) {
        dirtyLines_[row] = false;
    }
}

bool TerminalBuffer::IsLineDirty(int row) const {
    return (row >= 0 && row < rows_) ? dirtyLines_[row] : false;
}

bool TerminalBuffer::HasDirtyLines() const {
    return std::any_of(dirtyLines_.begin(), dirtyLines_.end(), [](bool d) { return d; });
}

void TerminalBuffer::AddToScrollback(const std::vector<TerminalCell>& line) {
    scrollback_.push_back(line);
    if (scrollback_.size() > MAX_SCROLLBACK) {
        scrollback_.pop_front();
    }
}

const std::vector<TerminalCell>& TerminalBuffer::GetScrollbackLine(int index) const {
    static std::vector<TerminalCell> empty;
    if (index >= 0 && index < static_cast<int>(scrollback_.size())) {
        return scrollback_[index];
    }
    return empty;
}

void TerminalBuffer::ClearScrollback() {
    scrollback_.clear();
}

// ============================================================================
// TerminalPanel Constructor/Destructor
// ============================================================================

TerminalPanel::TerminalPanel() {
    wchar_t path[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, path);
    currentDirectory_ = path;
}

TerminalPanel::~TerminalPanel() {
    Shutdown();
}

// ============================================================================
// libvterm Callbacks - C'est ici que la magie opère !
// ============================================================================

int TerminalPanel::OnDamage(VTermRect rect, void* user) {
    auto* self = static_cast<TerminalPanel*>(user);
    if (!self) {
        Logger::Instance().Log(std::wstring(L"TerminalPanel::OnDamage - user == nullptr"));
        return 0;
    }
    {
        wchar_t buf[128];
        swprintf_s(buf, L"TerminalPanel::OnDamage - rect(%d,%d - %d,%d)", rect.start_row, rect.start_col, rect.end_row, rect.end_col);
        Logger::Instance().Log(std::wstring(buf));
    }
    std::lock_guard<std::mutex> lock(self->bufferMutex_);

    // Marquer seulement les lignes modifiées comme dirty
    for (int row = rect.start_row; row < rect.end_row && row < self->bufferRows_; ++row) {
        if (self->buffer_) {
            self->buffer_->MarkDirty(row);
        }
    }

    return 1;
}

int TerminalPanel::OnMoveCursor(VTermPos pos, VTermPos oldpos, int visible, void* user) {
    auto* self = static_cast<TerminalPanel*>(user);
    if (!self) {
        Logger::Instance().Log(std::wstring(L"TerminalPanel::OnMoveCursor - user == nullptr"));
        return 0;
    }
    {
        wchar_t buf[128];
        swprintf_s(buf, L"TerminalPanel::OnMoveCursor - pos(%d,%d) old(%d,%d) vis=%d", pos.row, pos.col, oldpos.row, oldpos.col, visible);
        Logger::Instance().Log(std::wstring(buf));
    }
    self->cursorRow_ = pos.row;
    self->cursorCol_ = pos.col;
    self->cursorVisible_ = (visible != 0);

    // Marquer les anciennes et nouvelles lignes du curseur comme dirty
    if (self->buffer_) {
        self->buffer_->MarkDirty(oldpos.row);
        self->buffer_->MarkDirty(pos.row);
    }

    return 1;
}

int TerminalPanel::OnSetTermProp(VTermProp prop, VTermValue* val, void* user) {
    (void)val;
    auto* self = static_cast<TerminalPanel*>(user);
    if (!self) {
        Logger::Instance().Log(std::wstring(L"TerminalPanel::OnSetTermProp - user == nullptr"));
        return 0;
    }
    wchar_t buf[128];
    swprintf_s(buf, L"TerminalPanel::OnSetTermProp - prop=%d", static_cast<int>(prop));
    Logger::Instance().Log(std::wstring(buf));
    return 1;
}

// Conversion couleur VTerm -> Windows COLORREF
COLORREF TerminalPanel::VTermColorToColorRef(const VTermColor& color) {
    if (VTERM_COLOR_IS_DEFAULT_FG(&color)) return 0xCCCCCC;
    if (VTERM_COLOR_IS_DEFAULT_BG(&color)) return 0x121212;
    
    if (VTERM_COLOR_IS_INDEXED(&color)) {
        int idx = color.indexed.idx;
        
        // Couleurs standard 0-15
        if (idx < 16) {
            static const COLORREF standard[] = {
                0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00,
                0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
                0x767676, 0xE74856, 0x16C60C, 0xF9F1A5,
                0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2
            };
            return standard[idx];
        }
        
        // Couleurs 256 (232-255 = grayscale)
        if (idx >= 232) {
            int gray = 8 + (idx - 232) * 10;
            return RGB(gray, gray, gray);
        }
        
        // Cube 6x6x6
        int ridx = idx - 16;
        int r = (ridx / 36) * 51;
        int g = ((ridx / 6) % 6) * 51;
        int b = (ridx % 6) * 51;
        return RGB(r, g, b);
    }
    
    // Couleur RGB directe
    return RGB(color.rgb.red, color.rgb.green, color.rgb.blue);
}

// ============================================================================
// Sync cells from libvterm to our buffer - MAGIC HAPPENS HERE
// ============================================================================

void TerminalPanel::SyncCellsFromVTerm() {
    if (TERMINAL_PANEL_DISABLED) return;
    Logger::Instance().Log(std::wstring(L"TerminalPanel::SyncCellsFromVTerm called"));
    if (!vtscreen_ || !buffer_) {
        Logger::Instance().Log(std::wstring(L"TerminalPanel::SyncCellsFromVTerm early exit: no vtscreen or buffer"));
        return;
    }
    
    for (int row = 0; row < bufferRows_; ++row) {
        if (!buffer_->IsLineDirty(row)) continue;
        
        for (int col = 0; col < bufferCols_; ++col) {
            VTermPos pos = {row, col};
            VTermScreenCell cell;
            vterm_screen_get_cell(vtscreen_, pos, &cell);
            
            TerminalCell tcell;
            tcell.ch = cell.chars[0] ? static_cast<wchar_t>(cell.chars[0]) : L' ';
            tcell.fgColor = VTermColorToColorRef(cell.fg);
            tcell.bgColor = VTermColorToColorRef(cell.bg);
            tcell.bold = cell.attrs.bold;
            tcell.underline = cell.attrs.underline;
            tcell.inverse = cell.attrs.reverse;
            
            if (tcell.inverse) {
                std::swap(tcell.fgColor, tcell.bgColor);
            }
            
            buffer_->SetCell(row, col, tcell);
        }
    }
}

// ============================================================================
// Initialization
// ============================================================================

void TerminalPanel::Initialize() {
    if (TERMINAL_PANEL_DISABLED) return;
    Logger::Instance().Log(L"TerminalPanel::Initialize() called");

    if (!buffer_) {
        buffer_ = std::make_unique<TerminalBuffer>(bufferCols_, bufferRows_);
    }

    // Créer libvterm instance
    vterm_ = vterm_new(bufferRows_, bufferCols_);
    if (!vterm_) {
        Logger::Instance().Log(L"TerminalPanel::Initialize() - vterm_new FAILED");
        return;
    }
    vterm_set_utf8(vterm_, 1);

    // Obtenir le screen
    vtscreen_ = vterm_obtain_screen(vterm_);
    if (!vtscreen_) {
        Logger::Instance().Log(L"TerminalPanel::Initialize() - vterm_obtain_screen FAILED");
        vterm_free(vterm_);
        vterm_ = nullptr;
        return;
    }

    // Configurer les callbacks
    VTermScreenCallbacks callbacks = {};
    callbacks.damage = OnDamage;
    callbacks.movecursor = OnMoveCursor;
    callbacks.settermprop = OnSetTermProp;

    vterm_screen_set_callbacks(vtscreen_, &callbacks, this);
    vterm_screen_reset(vtscreen_, 1);

    // Marquer tout comme dirty pour le premier dessin
    for (int r = 0; r < bufferRows_; ++r) {
        buffer_->MarkDirty(r);
    }

    forceFullRedraw_ = true;

    if (!processRunning_) {
        if (!StartConPTY()) {
            Logger::Instance().Log(L"TerminalPanel::Initialize() - StartConPTY FAILED");
            // don't return; we allow a headless vterm without conpty for testing
        }
    }

    Logger::Instance().Log(L"TerminalPanel::Initialize() completed");
}

void TerminalPanel::Shutdown() {
    if (TERMINAL_PANEL_DISABLED) return;
    StopConPTY();
    {
        wchar_t buf[128];
        swprintf_s(buf, L"TerminalPanel::UpdateLayout completed - cols=%d rows=%d physicalH=%d", bufferCols_, bufferRows_, state_.physicalHeight);
        Logger::Instance().Log(std::wstring(buf));
    }

    if (vterm_) {
        vterm_free(vterm_);
        vterm_ = nullptr;
        vtscreen_ = nullptr;
    }

    buffer_.reset();
}

void TerminalPanel::SetVisible(bool v) {
    if (TERMINAL_PANEL_DISABLED) { visible_ = false; return; }
    {
        wchar_t buf[128];
        swprintf_s(buf, L"TerminalPanel::SetVisible(%d) called - vterm=%p, buffer=%p", v, (void*)vterm_, (void*)buffer_.get());
        Logger::Instance().Log(std::wstring(buf));
    }
    visible_ = v;
    if (v) {
        state_.scrollOffset = 0;
        forceFullRedraw_ = true;

        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (buffer_) {
            for (int r = 0; r < bufferRows_; ++r) {
                buffer_->MarkDirty(r);
            }
        }
    } else {
        focused_ = false;
    }
    Logger::Instance().Log(std::wstring(L"TerminalPanel::SetVisible completed"));
}

void TerminalPanel::Unfocus() {
    if (focused_) {
        Logger::Instance().Log(L"TerminalPanel: Unfocus called");
    }
    focused_ = false;
}

void TerminalPanel::SetFocused(bool focused) {
    focused_ = focused;
    if (focused_) {
        lastCursorBlink_ = GetTickCount();
        cursorVisible_ = true;
    } else {
        // clear selection when losing focus
        selection_.Clear();
        cursorVisible_ = false;
    }
    forceFullRedraw_ = true;
}

void TerminalPanel::ToggleVisible() {
    if (TERMINAL_PANEL_DISABLED) { visible_ = false; return; }
    Logger::Instance().Log(std::wstring(L"TerminalPanel::ToggleVisible called"));
    SetVisible(!visible_);
    Logger::Instance().Log(std::wstring(L"TerminalPanel::ToggleVisible completed"));
}

void TerminalPanel::UpdateLayout(HWND hwnd, float editorLeft, float editorBottom, 
                                 float editorRight, float footerTop) {
    {
        wchar_t buf[128];
        swprintf_s(buf, L"TerminalPanel::UpdateLayout called - editorLeft=%.1f editorRight=%.1f footerTop=%.1f", editorLeft, editorRight, footerTop);
        Logger::Instance().Log(std::wstring(buf));
    }
    (void)editorBottom;
    
    UINT dpi = GetDpiForWindow(hwnd);
    float scale = dpi / 96.0f;
    
    state_.physicalHeight = static_cast<int>(state_.logicalHeight * scale);
    
    state_.leftEdge = editorLeft;
    state_.rightEdge = editorRight;
    state_.bottomEdge = footerTop;
    state_.topEdge = footerTop - state_.physicalHeight;
    
    float contentWidth = state_.rightEdge - state_.leftEdge - state_.padding * 2;
    float contentHeight = state_.bottomEdge - state_.topEdge - state_.titleHeight - state_.padding * 2;
    
    int newCols = std::max(20, static_cast<int>(contentWidth / state_.charWidth));
    int newRows = std::max(5, static_cast<int>(contentHeight / state_.lineHeight));
    
    if (newCols != bufferCols_ || newRows != bufferRows_) {
        bufferCols_ = newCols;
        bufferRows_ = newRows;
        
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (buffer_) {
            buffer_->Resize(bufferCols_, bufferRows_);
        }
        
        // Resize libvterm
        if (vterm_) {
            vterm_set_size(vterm_, bufferRows_, bufferCols_);
        }
        
        ResizeConPTY();
        forceFullRedraw_ = true;
    }
    Logger::Instance().Log(std::wstring(L"TerminalPanel::UpdateLayout completed"));
}

// ============================================================================
// Drawing (identique à avant)
// ============================================================================

void TerminalPanel::Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd) {
    if (TERMINAL_PANEL_DISABLED) return;
    Logger::Instance().Log(std::wstring(L"TerminalPanel::Draw called"));
    if (!visible_ || !ctx) {
        Logger::Instance().Log(std::wstring(L"TerminalPanel::Draw early exit: not visible or no ctx"));
        return;
    }
    
    DWORD now = GetTickCount();
    
    if (lastRenderTime_ == 0) {
        forceFullRedraw_ = true;
    }
    
    bool hasDirtyContent = false;
    bool needsCursorUpdate = false;
    
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        hasDirtyContent = (buffer_ && buffer_->HasDirtyLines()) || forceFullRedraw_;
        
        if (focused_ && (now - lastCursorBlink_) > 500) {
            needsCursorUpdate = true;
        }
        
        if (needsCursorUpdate && buffer_) {
            buffer_->MarkDirty(cursorRow_);
            hasDirtyContent = true;
        }
    }
    
    bool needsFullDraw = forceFullRedraw_ || hasDirtyContent || needsCursorUpdate || state_.isResizing;
    
    if (!needsFullDraw && (now - lastRenderTime_) < MIN_RENDER_INTERVAL_MS) {
        Logger::Instance().Log(std::wstring(L"TerminalPanel::Draw throttled, exiting"));
        return;
    }
    
    lastRenderTime_ = now;
    
    bool willDrawContent = forceFullRedraw_ || hasDirtyContent || needsCursorUpdate || state_.isResizing;
    DrawBackground(ctx, willDrawContent);
    DrawTopBorder(ctx);
    DrawTitleBar(ctx, dwrite);
    
    if (forceFullRedraw_) {
        DrawContent(ctx, dwrite, true);
        forceFullRedraw_ = false;
    } else if (hasDirtyContent) {
        DrawContent(ctx, dwrite, false);
    }
    
    if (focused_) {
        float contentLeft = state_.leftEdge + state_.padding;
        float contentTop = state_.topEdge + state_.titleHeight + state_.padding;
        DrawCursor(ctx, contentLeft, contentTop);
    }
    Logger::Instance().Log(std::wstring(L"TerminalPanel::Draw completed"));
}

void TerminalPanel::DrawCursor(ID2D1RenderTarget* ctx, float contentLeft, float contentTop) {
    if (!focused_) return;
    
    DWORD now = GetTickCount();
    if (now - lastCursorBlink_ > 500) {
        cursorVisible_ = !cursorVisible_;
        lastCursorBlink_ = now;
    }
    
    if (!cursorVisible_) return;
    
    float x = contentLeft + cursorCol_ * state_.charWidth;
    float y = contentTop + cursorRow_ * state_.lineHeight;
    
    ID2D1SolidColorBrush* cursorBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0xCCCCCC, 0.7f), &cursorBrush);
    
    if (cursorBrush) {
        D2D1_RECT_F cursorRect = D2D1::RectF(x, y, x + state_.charWidth, y + state_.lineHeight);
        ctx->FillRectangle(cursorRect, cursorBrush);
        cursorBrush->Release();
    }
}

void TerminalPanel::DrawBackground(ID2D1RenderTarget* ctx, bool clearContentArea) {
    ID2D1SolidColorBrush* bgBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x121212), &bgBrush);

    if (bgBrush) {
        D2D1_RECT_F panelRect = D2D1::RectF(state_.leftEdge, state_.topEdge,
                                            state_.rightEdge, state_.bottomEdge);
        if (clearContentArea) {
            ctx->FillRectangle(panelRect, bgBrush);
        } else {
            D2D1_RECT_F titleRect = D2D1::RectF(state_.leftEdge, state_.topEdge,
                                               state_.rightEdge, state_.topEdge + state_.titleHeight + state_.padding);
            ctx->FillRectangle(titleRect, bgBrush);
            D2D1_RECT_F topBorder = D2D1::RectF(state_.leftEdge, state_.topEdge, state_.rightEdge, state_.topEdge + 1.0f);
            ctx->FillRectangle(topBorder, bgBrush);
        }
        bgBrush->Release();
    }
}

void TerminalPanel::DrawTopBorder(ID2D1RenderTarget* ctx) {
    ID2D1SolidColorBrush* borderBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x303030), &borderBrush);
    
    if (borderBrush) {
        D2D1_RECT_F border = D2D1::RectF(state_.leftEdge, state_.topEdge, 
                                         state_.rightEdge, state_.topEdge + 1.0f);
        ctx->FillRectangle(border, borderBrush);
        borderBrush->Release();
    }
}

void TerminalPanel::DrawTitleBar(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite) {
    ID2D1SolidColorBrush* titleBgBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x1E1E1E), &titleBgBrush);
    
    if (titleBgBrush) {
        D2D1_RECT_F titleRect = D2D1::RectF(state_.leftEdge, state_.topEdge, 
                                            state_.rightEdge, state_.topEdge + state_.titleHeight);
        ctx->FillRectangle(titleRect, titleBgBrush);
        titleBgBrush->Release();
    }
    
    IDWriteTextFormat* textFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f, L"en-us", &textFormat);
    
    if (textFormat) {
        ID2D1SolidColorBrush* textBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0xCCCCCC), &textBrush);
        
        if (textBrush) {
            std::wstring title = L"TERMINAL (libvterm)";
            D2D1_RECT_F textRect = D2D1::RectF(state_.leftEdge + 12.0f, state_.topEdge + 6.0f,
                                              state_.rightEdge - 12.0f, state_.topEdge + state_.titleHeight);
            ctx->DrawTextW(title.c_str(), (UINT32)title.length(), textFormat, textRect, textBrush);
            textBrush->Release();
        }
        textFormat->Release();
    }
}

void TerminalPanel::DrawContent(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, bool forceAll) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    if (!buffer_) return;
    
    // SYNC depuis libvterm !
    SyncCellsFromVTerm();
    
    float contentLeft = state_.leftEdge + state_.padding;
    float contentTop = state_.topEdge + state_.titleHeight + state_.padding;
    
    IDWriteTextFormat* textFormat = nullptr;
    dwrite->CreateTextFormat(L"Consolas", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             13.0f, L"en-us", &textFormat);
    
    if (!textFormat) return;
    
    DrawSelection(ctx, contentLeft, contentTop);
    
    ID2D1SolidColorBrush* fgBrush = nullptr;
    ID2D1SolidColorBrush* bgBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0xCCCCCC), &fgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x121212), &bgBrush);
    
    if (!fgBrush || !bgBrush) {
        if (fgBrush) fgBrush->Release();
        if (bgBrush) bgBrush->Release();
        textFormat->Release();
        return;
    }
    
    for (int row = 0; row < bufferRows_; ++row) {
        if (!forceAll && !buffer_->IsLineDirty(row)) {
            continue;
        }
        
        float y = contentTop + row * state_.lineHeight;
        
        // Clear line background
        ID2D1SolidColorBrush* clearBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0x121212), &clearBrush);
        if (clearBrush) {
            D2D1_RECT_F clearRect = D2D1::RectF(
                contentLeft, y, 
                contentLeft + bufferCols_ * state_.charWidth, 
                y + state_.lineHeight
            );
            ctx->FillRectangle(clearRect, clearBrush);
            clearBrush->Release();
        }
        
        // Draw cell backgrounds
        for (int col = 0; col < bufferCols_; ++col) {
            const TerminalCell& cell = buffer_->GetCell(row, col);
            
            if (cell.bgColor != 0x121212) {
                float x = contentLeft + col * state_.charWidth;
                bgBrush->SetColor(D2D1::ColorF(cell.bgColor));
                D2D1_RECT_F bgRect = D2D1::RectF(x, y, x + state_.charWidth, y + state_.lineHeight);
                ctx->FillRectangle(bgRect, bgBrush);
            }
        }
        
        // Draw text runs
        int col = 0;
        while (col < bufferCols_) {
            const TerminalCell& startCell = buffer_->GetCell(row, col);
            COLORREF currentColor = startCell.fgColor;
            
            int runStart = col;
            std::wstring runText;
            
            while (col < bufferCols_) {
                const TerminalCell& cell = buffer_->GetCell(row, col);
                if (cell.fgColor != currentColor) break;
                
                runText += (cell.ch != 0) ? cell.ch : L' ';
                col++;
            }
            
            if (!runText.empty()) {
                fgBrush->SetColor(D2D1::ColorF(currentColor));
                float x = contentLeft + runStart * state_.charWidth;
                D2D1_RECT_F textRect = D2D1::RectF(x, y, 
                                                   x + runText.length() * state_.charWidth,
                                                   y + state_.lineHeight);
                ctx->DrawTextW(runText.c_str(), (UINT32)runText.length(), 
                              textFormat, textRect, fgBrush);
            }
        }
        
        buffer_->ClearDirtyFlag(row);
    }
    
    fgBrush->Release();
    bgBrush->Release();
    textFormat->Release();
}

void TerminalPanel::DrawSelection(ID2D1RenderTarget* ctx, float contentLeft, float contentTop) {
    if (!selection_.HasSelection()) return;
    
    ID2D1SolidColorBrush* selBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x264F78), &selBrush);
    
    if (!selBrush) return;
    
    int startRow = selection_.startRow, startCol = selection_.startCol;
    int endRow = selection_.endRow, endCol = selection_.endCol;
    
    if (startRow > endRow || (startRow == endRow && startCol > endCol)) {
        std::swap(startRow, endRow);
        std::swap(startCol, endCol);
    }
    
    for (int row = startRow; row <= endRow; ++row) {
        int c1 = (row == startRow) ? startCol : 0;
        int c2 = (row == endRow) ? endCol : bufferCols_;
        
        float x1 = contentLeft + c1 * state_.charWidth;
        float x2 = contentLeft + c2 * state_.charWidth;
        float y = contentTop + row * state_.lineHeight;
        
        D2D1_RECT_F selRect = D2D1::RectF(x1, y, x2, y + state_.lineHeight);
        ctx->FillRectangle(selRect, selBrush);
    }
    
    selBrush->Release();
}

// ============================================================================
// Hit Testing & Mouse (identique)
// ============================================================================

bool TerminalPanel::IsPointInPanel(POINT pt) const {
    if (!visible_) return false;
    return (pt.x >= state_.leftEdge && pt.x <= state_.rightEdge &&
            pt.y >= state_.topEdge && pt.y <= state_.bottomEdge);
}

bool TerminalPanel::IsPointInResizeZone(POINT pt) const {
    if (!visible_) return false;
    return (pt.x >= state_.leftEdge && pt.x <= state_.rightEdge &&
            pt.y >= (int)(state_.topEdge - RESIZE_ZONE_HEIGHT) &&
            pt.y <= (int)(state_.topEdge + RESIZE_ZONE_HEIGHT));
}

std::pair<int, int> TerminalPanel::PointToCell(POINT pt) const {
    float contentLeft = state_.leftEdge + state_.padding;
    float contentTop = state_.topEdge + state_.titleHeight + state_.padding;
    
    int col = static_cast<int>((pt.x - contentLeft) / state_.charWidth);
    int row = static_cast<int>((pt.y - contentTop) / state_.lineHeight);
    
    col = std::clamp(col, 0, bufferCols_ - 1);
    row = std::clamp(row, 0, bufferRows_ - 1);
    
    return {row, col};
}

bool TerminalPanel::HandleResizeMouseMove(HWND hwnd, POINT clientPoint) {
    (void)hwnd;
    bool changed = false;

    if (state_.isResizing) {
        int delta = state_.resizeStartY - clientPoint.y;
        int newHeight = state_.resizeStartHeight + delta;
        int clamped = std::clamp(newHeight, state_.minHeight, state_.maxHeight);
        
        if (clamped != state_.logicalHeight) {
            state_.logicalHeight = clamped;
            forceFullRedraw_ = true;
            changed = true;
        }
    }

    bool wasHovering = state_.isHoveringResizeZone;
    state_.isHoveringResizeZone = IsPointInResizeZone(clientPoint);

    if (state_.isHoveringResizeZone != wasHovering) {
        SetCursor(LoadCursor(NULL, state_.isHoveringResizeZone ? IDC_SIZENS : IDC_ARROW));
    }

    return changed;
}

bool TerminalPanel::HandleResizeLeftButtonDown(HWND hwnd, POINT clientPoint) {
    if (IsPointInResizeZone(clientPoint)) {
        state_.isResizing = true;
        state_.resizeStartY = clientPoint.y;
        state_.resizeStartHeight = state_.logicalHeight;
        SetCapture(hwnd);
        return true;
    }
    return false;
}

bool TerminalPanel::HandleResizeLeftButtonUp(HWND hwnd) {
    (void)hwnd;
    if (state_.isResizing) {
        state_.isResizing = false;
        ReleaseCapture();
        return true;
    }
    return false;
}

bool TerminalPanel::OnMouseMove(HWND hwnd, POINT clientPoint) {
    if (TERMINAL_PANEL_DISABLED) return false;
    bool needsRedraw = HandleResizeMouseMove(hwnd, clientPoint);
    
    if (selection_.selecting) {
        auto [row, col] = PointToCell(clientPoint);
        
        if (row != selection_.endRow || col != selection_.endCol) {
            UpdateSelection(row, col);
            needsRedraw = true;
        }
    }
    
    if (needsRedraw) {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        forceFullRedraw_ = true;
    }

    return needsRedraw;
}

void TerminalPanel::OnMouseWheel(HWND hwnd, int delta) {
    if (TERMINAL_PANEL_DISABLED) return;
    (void)hwnd;
    std::lock_guard<std::mutex> lock(bufferMutex_);
    if (!buffer_) return;

    int linesToScroll = delta / WHEEL_DELTA * 3;
    int maxScroll = buffer_->GetScrollbackSize();

    state_.scrollOffset = std::clamp(state_.scrollOffset - linesToScroll, 0, maxScroll);
    forceFullRedraw_ = true;
}

// ============================================================================
// Selection
// ============================================================================

void TerminalPanel::StartSelection(int row, int col) {
    selection_.startRow = row;
    selection_.startCol = col;
    selection_.endRow = row;
    selection_.endCol = col;
    selection_.selecting = true;
    selection_.active = false;
}

void TerminalPanel::UpdateSelection(int row, int col) {
    if (selection_.selecting) {
        selection_.endRow = row;
        selection_.endCol = col;
        selection_.active = true;
    }
}

void TerminalPanel::EndSelection() {
    selection_.selecting = false;
}

void TerminalPanel::SelectAll() {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    selection_.startRow = 0;
    selection_.startCol = 0;
    selection_.endRow = bufferRows_ - 1;
    selection_.endCol = bufferCols_;
    selection_.active = true;
    selection_.selecting = false;
    forceFullRedraw_ = true;
}

std::wstring TerminalPanel::GetSelectedText() const {
    if (!selection_.HasSelection() || !buffer_) return L"";
    
    std::wstring result;
    
    int startRow = selection_.startRow, startCol = selection_.startCol;
    int endRow = selection_.endRow, endCol = selection_.endCol;
    
    if (startRow > endRow || (startRow == endRow && startCol > endCol)) {
        std::swap(startRow, endRow);
        std::swap(startCol, endCol);
    }
    
    for (int row = startRow; row <= endRow; ++row) {
        int c1 = (row == startRow) ? startCol : 0;
        int c2 = (row == endRow) ? endCol : bufferCols_;
        
        for (int col = c1; col < c2; ++col) {
            result += buffer_->GetCell(row, col).ch;
        }
        
        if (row < endRow) result += L'\n';
    }
    
    return result;
}

// ============================================================================
// Keyboard Events - Input vers libvterm
// ============================================================================

void TerminalPanel::OnChar(wchar_t ch) {
    if (TERMINAL_PANEL_DISABLED) return;
    if (!focused_ || !processRunning_ || !vterm_) return;
    
    char utf8[4];
    int len = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, sizeof(utf8), NULL, NULL);
    if (len > 0) {
        WriteToConPTY(utf8, len);
    }
}
void TerminalPanel::OnKeyDown(WPARAM key) {
    {
        wchar_t buf[128];
        swprintf_s(buf, L"TerminalPanel::OnKeyDown - focused=%d, vterm=%p, running=%d", focused_, (void*)vterm_, processRunning_);
        Logger::Instance().Log(std::wstring(buf));
    }
    if (TERMINAL_PANEL_DISABLED) return;
    if (!focused_ || !vterm_ || !processRunning_) return;
    
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    
    if (ctrl) {
        switch (key) {
            case 'C':
                if (selection_.HasSelection()) {
                    Copy();
                } else {
                    char ctrlC = 3;
                    WriteToConPTY(&ctrlC, 1);
                }
                return;
            case 'V':
                Paste();
                return;
            case 'A':
                SelectAll();
                return;
            case 'L':
                Clear();
                return;
            case 'U':
                // Ctrl+U -> send ASCII 0x15 (NAK / Ctrl+U) to the pty
                {
                    char ctrlU = 0x15;
                    wchar_t buf[128];
                    swprintf_s(buf, L"TerminalPanel::OnKeyDown - Ctrl+U intercepted, sending to ConPTY");
                    Logger::Instance().Log(std::wstring(buf));
                    WriteToConPTY(&ctrlU, 1);
                }
                return;
        }
    }
    
    // libvterm peut générer les séquences pour nous !
    VTermModifier mod = VTERM_MOD_NONE;
    if (ctrl) mod = (VTermModifier)(mod | VTERM_MOD_CTRL);
    if (GetKeyState(VK_SHIFT) & 0x8000) mod = (VTermModifier)(mod | VTERM_MOD_SHIFT);
    if (GetKeyState(VK_MENU) & 0x8000) mod = (VTermModifier)(mod | VTERM_MOD_ALT);
    
    VTermKey vtkey = VTERM_KEY_NONE;
    
    // FIX: Cast explicite pour VTermKey enum (MSVC C++ strict)
    switch (key) {
        case VK_UP:    vtkey = static_cast<VTermKey>(VTERM_KEY_UP); break;
        case VK_DOWN:  vtkey = static_cast<VTermKey>(VTERM_KEY_DOWN); break;
        case VK_LEFT:  vtkey = static_cast<VTermKey>(VTERM_KEY_LEFT); break;
        case VK_RIGHT: vtkey = static_cast<VTermKey>(VTERM_KEY_RIGHT); break;
        case VK_HOME:  vtkey = static_cast<VTermKey>(VTERM_KEY_HOME); break;
        case VK_END:   vtkey = static_cast<VTermKey>(VTERM_KEY_END); break;
        case VK_INSERT: vtkey = static_cast<VTermKey>(VTERM_KEY_INS); break;
        case VK_DELETE: vtkey = static_cast<VTermKey>(VTERM_KEY_DEL); break;
        case VK_PRIOR: vtkey = static_cast<VTermKey>(VTERM_KEY_PAGEUP); break;
        case VK_NEXT:  vtkey = static_cast<VTermKey>(VTERM_KEY_PAGEDOWN); break;
        case VK_F1:  vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(1)); break;
        case VK_F2:  vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(2)); break;
        case VK_F3:  vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(3)); break;
        case VK_F4:  vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(4)); break;
        case VK_F5:  vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(5)); break;
        case VK_F6:  vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(6)); break;
        case VK_F7:  vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(7)); break;
        case VK_F8:  vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(8)); break;
        case VK_F9:  vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(9)); break;
        case VK_F10: vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(10)); break;
        case VK_F11: vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(11)); break;
        case VK_F12: vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(12)); break;
        case VK_BACK: vtkey = static_cast<VTermKey>(VTERM_KEY_BACKSPACE); break;
        case VK_TAB:  vtkey = static_cast<VTermKey>(VTERM_KEY_TAB); break;
        case VK_RETURN: vtkey = static_cast<VTermKey>(VTERM_KEY_ENTER); break;
        case VK_ESCAPE: vtkey = static_cast<VTermKey>(VTERM_KEY_ESCAPE); break;
    }
    
    if (vtkey != VTERM_KEY_NONE) {
        // libvterm génère la séquence correcte pour nous !
        vterm_keyboard_key(vterm_, vtkey, mod);
        
        // Récupérer l'output et l'envoyer à ConPTY
        size_t len = vterm_output_get_buffer_current(vterm_);
        if (len > 0) {
            char buf[256];
            len = vterm_output_read(vterm_, buf, sizeof(buf));
            if (len > 0) {
                WriteToConPTY(buf, len);
            }
        }
    }
}

// ============================================================================
// Clipboard
// ============================================================================

void TerminalPanel::Copy() {
    if (TERMINAL_PANEL_DISABLED) return;
    std::wstring text = GetSelectedText();
    if (text.empty()) return;
    
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        
        size_t size = (text.length() + 1) * sizeof(wchar_t);
        HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, size);
        
        if (hGlobal) {
            wchar_t* pGlobal = static_cast<wchar_t*>(GlobalLock(hGlobal));
            if (pGlobal) {
                wcscpy_s(pGlobal, text.length() + 1, text.c_str());
                GlobalUnlock(hGlobal);
                SetClipboardData(CF_UNICODETEXT, hGlobal);
            }
        }
        
        CloseClipboard();
    }
    
    selection_.Clear();
    forceFullRedraw_ = true;
}

void TerminalPanel::Paste() {
    if (TERMINAL_PANEL_DISABLED) return;
    if (!OpenClipboard(NULL)) return;
    
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        wchar_t* pData = static_cast<wchar_t*>(GlobalLock(hData));
        if (pData) {
            WriteToConPTY(pData);
            GlobalUnlock(hData);
        }
    }
    
    CloseClipboard();
}

void TerminalPanel::Clear() {
    if (TERMINAL_PANEL_DISABLED) return;
    std::lock_guard<std::mutex> lock(bufferMutex_);
    if (buffer_) {
        buffer_->Clear();
        buffer_->ClearScrollback();
    }
    
    // Reset libvterm
    if (vtscreen_) {
        vterm_screen_reset(vtscreen_, 1);
    }
    
    cursorRow_ = 0;
    cursorCol_ = 0;
    state_.scrollOffset = 0;
    forceFullRedraw_ = true;
}

void TerminalPanel::KillProcess() {
    if (TERMINAL_PANEL_DISABLED) return;
    if (hProcess_ && processRunning_) {
        TerminateProcess(hProcess_, 1);
    }
}

// ============================================================================
// ConPTY Process Management (identique)
// ============================================================================

bool TerminalPanel::StartConPTY() {
    if (TERMINAL_PANEL_DISABLED) return false;
    if (processRunning_) return true;
    
    HANDLE hPipeInRead = nullptr, hPipeInWrite = nullptr;
    HANDLE hPipeOutRead = nullptr, hPipeOutWrite = nullptr;
    
    if (!CreatePipe(&hPipeInRead, &hPipeInWrite, NULL, 0)) {
        return false;
    }
    if (!CreatePipe(&hPipeOutRead, &hPipeOutWrite, NULL, 0)) {
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeInWrite);
        return false;
    }
    
    COORD size = { (SHORT)bufferCols_, (SHORT)bufferRows_ };
    HRESULT hr = CreatePseudoConsole(size, hPipeInRead, hPipeOutWrite, 0, &hPC_);
    
    if (FAILED(hr)) {
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        CloseHandle(hPipeOutWrite);
        return false;
    }
    
    CloseHandle(hPipeInRead);
    CloseHandle(hPipeOutWrite);
    
    hPipeOut_ = hPipeInWrite;
    hPipeIn_ = hPipeOutRead;
    
    STARTUPINFOEXW siEx = {};
    siEx.StartupInfo.cb = sizeof(siEx);
    
    SIZE_T attrListSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);
    
    siEx.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrListSize);
    if (!siEx.lpAttributeList) {
        ClosePseudoConsole(hPC_);
        hPC_ = nullptr;
        CloseHandle(hPipeOut_);
        CloseHandle(hPipeIn_);
        return false;
    }
    
    InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrListSize);
    UpdateProcThreadAttribute(siEx.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                              hPC_, sizeof(HPCON), NULL, NULL);
    
    PROCESS_INFORMATION pi = {};
    wchar_t cmdLine[] = L"powershell.exe";
    
    BOOL success = CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                                  EXTENDED_STARTUPINFO_PRESENT, NULL,
                                  currentDirectory_.c_str(), &siEx.StartupInfo, &pi);
    
    DeleteProcThreadAttributeList(siEx.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
    
    if (!success) {
        ClosePseudoConsole(hPC_);
        hPC_ = nullptr;
        CloseHandle(hPipeOut_);
        CloseHandle(hPipeIn_);
        return false;
    }
    
    hProcess_ = pi.hProcess;
    hThread_ = pi.hThread;
    processRunning_ = true;
    
    readThread_ = std::thread(&TerminalPanel::ReadThread, this);
    
    return true;
}

void TerminalPanel::StopConPTY() {
    if (TERMINAL_PANEL_DISABLED) return;
    processRunning_ = false;
    
    if (hPC_) {
        ClosePseudoConsole(hPC_);
        hPC_ = nullptr;
    }
    
    if (hPipeOut_) {
        CloseHandle(hPipeOut_);
        hPipeOut_ = nullptr;
    }
    
    if (hPipeIn_) {
        CloseHandle(hPipeIn_);
        hPipeIn_ = nullptr;
    }
    
    if (readThread_.joinable()) {
        readThread_.join();
    }
    
    if (hProcess_) {
        TerminateProcess(hProcess_, 0);
        CloseHandle(hProcess_);
        hProcess_ = nullptr;
    }
    
    if (hThread_) {
        CloseHandle(hThread_);
        hThread_ = nullptr;
    }
}

void TerminalPanel::ResizeConPTY() {
    if (TERMINAL_PANEL_DISABLED) return;
    if (hPC_) {
        COORD size = { (SHORT)bufferCols_, (SHORT)bufferRows_ };
        ResizePseudoConsole(hPC_, size);
    }
}

void TerminalPanel::WriteToConPTY(const std::wstring& text) {
    if (TERMINAL_PANEL_DISABLED) return;
    if (!hPipeOut_ || !processRunning_) return;

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, NULL, 0, NULL, NULL);
    if (utf8Len > 0) {
        std::string utf8(utf8Len, 0);
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), utf8Len, NULL, NULL);
        WriteToConPTY(utf8.c_str(), utf8.length() - 1);
    }
}

void TerminalPanel::WriteToConPTY(const char* data, size_t len) {
    if (TERMINAL_PANEL_DISABLED) return;
    if (!hPipeOut_ || !processRunning_ || len == 0) return;
    
    DWORD written;
    WriteFile(hPipeOut_, data, (DWORD)len, &written, NULL);
}

void TerminalPanel::ReadThread() {
    char buffer[4096];
    DWORD bytesRead;
    
    if (TERMINAL_PANEL_DISABLED) return;
    while (processRunning_) {
        if (ReadFile(hPipeIn_, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
            {
                wchar_t buf[128];
                swprintf_s(buf, L"TerminalPanel::ReadThread read %u bytes", (unsigned)bytesRead);
                Logger::Instance().Log(std::wstring(buf));
            }
            // Copy data to heap and post to main window to process on UI thread
            char* heapBuf = static_cast<char*>(malloc(bytesRead));
            if (heapBuf) {
                memcpy(heapBuf, buffer, bytesRead);
                HWND mainWnd = FindWindowW(L"NebulaTextWindowClass", NULL);
                if (mainWnd) {
                    PostMessageW(mainWnd, WM_USER + 201, (WPARAM)bytesRead, (LPARAM)heapBuf);
                } else {
                    // Fallback: process inline if we cannot find main window
                    ProcessOutput(heapBuf, bytesRead);
                    free(heapBuf);
                }
            }
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                processRunning_ = false;
                break;
            }
            Sleep(10);
        }
    }
}

// ============================================================================
// THE MAGIC: Process output through libvterm
// ============================================================================

void TerminalPanel::ProcessOutput(const char* data, size_t len) {
    if (TERMINAL_PANEL_DISABLED) return;
    std::lock_guard<std::mutex> lock(bufferMutex_);
    {
        wchar_t buf[128];
        swprintf_s(buf, L"TerminalPanel::ProcessOutput called - len=%zu vterm=%p", len, (void*)vterm_);
        Logger::Instance().Log(std::wstring(buf));
    }
    if (!vterm_) {
        Logger::Instance().Log(std::wstring(L"TerminalPanel::ProcessOutput early exit - vterm is null"));
        return;
    }

    vterm_input_write(vterm_, data, len);

    Logger::Instance().Log(std::wstring(L"TerminalPanel::ProcessOutput completed"));
}

void TerminalPanel::HandleConPTYOutput(char* buf, size_t len) {
    if (TERMINAL_PANEL_DISABLED) { if (buf) free(buf); return; }
    if (!buf || len == 0) {
        if (buf) free(buf);
        return;
    }
    ProcessOutput(buf, len);
    free(buf);
}

void TerminalPanel::OnLeftButtonDown(HWND hwnd, POINT clientPoint) {
    if (TERMINAL_PANEL_DISABLED) return;
    if (HandleResizeLeftButtonDown(hwnd, clientPoint)) return;
    
    focused_ = true;
    
    if (IsPointInPanel(clientPoint)) {
        auto [row, col] = PointToCell(clientPoint);
        StartSelection(row, col);
        SetCapture(hwnd);
    }
}

void TerminalPanel::OnLeftButtonUp(HWND hwnd) {
    if (TERMINAL_PANEL_DISABLED) return;
    if (HandleResizeLeftButtonUp(hwnd)) return;
    
    if (selection_.selecting) {
        EndSelection();
        ReleaseCapture();
    }
}

void TerminalPanel::OnRightButtonDown(HWND hwnd, POINT clientPoint) {
    if (TERMINAL_PANEL_DISABLED) return;
    if (IsPointInPanel(clientPoint)) {
        focused_ = true;
        Paste();
    }
}