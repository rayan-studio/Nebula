#include "TerminalSession.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// --- libvterm include UNIQUEMENT ici ---
#pragma push_macro("IN")
#pragma push_macro("OUT")
#pragma push_macro("DELETE")
#pragma push_macro("ERROR")
#pragma push_macro("min")
#pragma push_macro("max")
#pragma push_macro("small")

#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif
#ifdef DELETE
#undef DELETE
#endif
#ifdef ERROR
#undef ERROR
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef small
#undef small
#endif

#include "vterm.h"

#pragma pop_macro("small")
#pragma pop_macro("max")
#pragma pop_macro("min")
#pragma pop_macro("ERROR")
#pragma pop_macro("DELETE")
#pragma pop_macro("OUT")
#pragma pop_macro("IN")

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

// ConPTY dynamic loading
typedef HRESULT(WINAPI* CreatePseudoConsole_t)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT(WINAPI* ResizePseudoConsole_t)(HPCON, COORD);
typedef VOID(WINAPI* ClosePseudoConsole_t)(HPCON);

static CreatePseudoConsole_t pCreatePseudoConsole = nullptr;
static ResizePseudoConsole_t pResizePseudoConsole = nullptr;
static ClosePseudoConsole_t  pClosePseudoConsole = nullptr;

static bool gConPTYLoaded = false;

static bool LoadConPTY()
{
    if (gConPTYLoaded) return (pCreatePseudoConsole && pResizePseudoConsole && pClosePseudoConsole);

    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel) return false;

    pCreatePseudoConsole = (CreatePseudoConsole_t)GetProcAddress(hKernel, "CreatePseudoConsole");
    pResizePseudoConsole = (ResizePseudoConsole_t)GetProcAddress(hKernel, "ResizePseudoConsole");
    pClosePseudoConsole  = (ClosePseudoConsole_t)GetProcAddress(hKernel, "ClosePseudoConsole");

    gConPTYLoaded = true;
    return pCreatePseudoConsole && pResizePseudoConsole && pClosePseudoConsole;
}

static float ClampF(float v, float a, float b)
{
    return (v < a) ? a : (v > b) ? b : v;
}

static std::string WideToUtf8Char(wchar_t ch)
{
    wchar_t w[2] = { ch, 0 };
    int len = WideCharToMultiByte(CP_UTF8, 0, w, 1, NULL, 0, NULL, NULL);
    if (len <= 0) return {};
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, 1, out.data(), len, NULL, NULL);
    return out;
}

TerminalSession::TerminalSession() {}
TerminalSession::~TerminalSession() { Shutdown(); }

void TerminalSession::StartReadThread()
{
    if (readerThread_.joinable())
        return;

    stopReader_.store(false);
    readerThread_ = std::thread(&TerminalSession::ReadLoop, this);
}

void TerminalSession::StopReadThread()
{
    stopReader_.store(true);
    if (readerThread_.joinable())
        readerThread_.join();
}

void TerminalSession::ReadLoop()
{
    if (!hOutR_ || !hwndOwner_)
        return;

    std::vector<char> buffer(4096);

    while (!stopReader_.load())
    {
        DWORD available = 0;
        if (!PeekNamedPipe(hOutR_, NULL, 0, NULL, &available, NULL))
            break;

        if (available == 0)
        {
            Sleep(10);
            continue;
        }

        DWORD toRead = std::min<DWORD>(available, (DWORD)buffer.size());
        DWORD read = 0;
        if (!ReadFile(hOutR_, buffer.data(), toRead, &read, NULL) || read == 0)
        {
            Sleep(1);
            continue;
        }

        char* heap = (char*)malloc(read);
        if (!heap)
            continue;
        memcpy(heap, buffer.data(), read);
        PostMessageW(hwndOwner_, WM_USER + 201, (WPARAM)read, (LPARAM)heap);
    }
}

bool TerminalSession::LoadConPTYOnce()
{
    conptyLoaded_ = LoadConPTY();
    return conptyLoaded_;
}

bool TerminalSession::Initialize(HWND hwndOwner, const std::wstring& startDir)
{
    if (initialized_) return true;

    hwndOwner_ = hwndOwner;

    if (!LoadConPTYOnce())
        return false;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), NULL, TRUE };

    if (!CreatePipe(&hInR_, &hInW_, &sa, 0)) return false;
    if (!CreatePipe(&hOutR_, &hOutW_, &sa, 0)) return false;

    COORD sz{ (SHORT)cols_, (SHORT)rows_ };
    HRESULT hr = pCreatePseudoConsole(sz, hInR_, hOutW_, 0, (HPCON*)&hPC_);
    if (FAILED(hr) || !hPC_) return false;

    if (!StartShellProcess(startDir)) return false;

    InitVTerm();
    initialized_ = true;
    pendingSnapToBottom_ = true;
    StartReadThread();
    return true;
}

void TerminalSession::Shutdown()
{
    StopReadThread();
    if (textFormat_) { textFormat_->Release(); textFormat_ = nullptr; }

    DestroyVTerm();
    CloseConPTY();

    initialized_ = false;
    pendingSnapToBottom_ = false;
    userScrolling_ = false;
}

bool TerminalSession::StartShellProcess(const std::wstring& startDir)
{
    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);

    SIZE_T attrListSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

    std::vector<BYTE> attrBuf(attrListSize);
    siex.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();

    if (!InitializeProcThreadAttributeList(siex.lpAttributeList, 1, 0, &attrListSize))
        return false;

    if (!UpdateProcThreadAttribute(
        siex.lpAttributeList,
        0,
        (DWORD_PTR)PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
        hPC_, sizeof(hPC_),
        NULL, NULL))
    {
        DeleteProcThreadAttributeList(siex.lpAttributeList);
        return false;
    }

    std::wstring workDir = startDir;
    if (workDir.empty())
    {
        wchar_t cur[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cur);
        workDir = cur;
    }

    std::wstring cmd = L"powershell.exe -NoLogo";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;

    BOOL ok = CreateProcessW(
        NULL,
        cmdline.data(),
        NULL, NULL,
        TRUE,
        flags,
        NULL,
        workDir.c_str(),
        &siex.StartupInfo,
        &pi);

    DeleteProcThreadAttributeList(siex.lpAttributeList);

    if (!ok) return false;

    CloseHandle(pi.hThread);
    hChild_ = pi.hProcess;
    return true;
}

void TerminalSession::CloseConPTY()
{
    if (hChild_) { CloseHandle(hChild_); hChild_ = NULL; }

    if (hPC_ && pClosePseudoConsole)
    {
        pClosePseudoConsole((HPCON)hPC_);
        hPC_ = NULL;
    }

    if (hInR_)  { CloseHandle(hInR_);  hInR_ = NULL; }
    if (hInW_)  { CloseHandle(hInW_);  hInW_ = NULL; }
    if (hOutR_) { CloseHandle(hOutR_); hOutR_ = NULL; }
    if (hOutW_) { CloseHandle(hOutW_); hOutW_ = NULL; }
}

void TerminalSession::WriteUtf8(const char* bytes, DWORD len)
{
    if (!initialized_ || !hInW_ || !bytes || len == 0) return;
    DWORD written = 0;
    WriteFile(hInW_, bytes, len, &written, NULL);
}

void TerminalSession::WriteVtSequence(const char* seq)
{
    if (!seq) return;
    WriteUtf8(seq, (DWORD)strlen(seq));
}

void TerminalSession::OnChar(wchar_t ch)
{
    if (!initialized_) return;

    if (suppressNextChar_)
    {
        suppressNextChar_ = false;
        if (ch == 0x03)
            return;
    }

    if (ch == L'\r' || ch == L'\n' || ch == L'\b' || ch == 0x1B || ch == L'\t')
        return;

    std::string u8 = WideToUtf8Char(ch);
    if (!u8.empty())
        WriteUtf8(u8.data(), (DWORD)u8.size());
}

void TerminalSession::OnKeyDown(WPARAM vk)
{
    if (!initialized_) return;

    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrl && (vk == 'C' || vk == 'c'))
    {
        if (CopySelectionToClipboard())
        {
            suppressNextChar_ = true;
            return;
        }
    }

    switch (vk)
    {
    case VK_RETURN: WriteUtf8("\r", 1); break;
    case VK_BACK:   WriteUtf8("\x08", 1); break;
    case VK_TAB:    WriteUtf8("\t", 1); break;

    case VK_UP:     WriteVtSequence("\x1b[A"); break;
    case VK_DOWN:   WriteVtSequence("\x1b[B"); break;
    case VK_RIGHT:  WriteVtSequence("\x1b[C"); break;
    case VK_LEFT:   WriteVtSequence("\x1b[D"); break;

    case VK_DELETE: WriteVtSequence("\x1b[3~"); break;
    case VK_HOME:   WriteVtSequence("\x1b[H"); break;
    case VK_END:    WriteVtSequence("\x1b[F"); break;
    default:
        break;
    }
}

std::wstring TerminalSession::BuildRowText(int row) const
{
    std::wstring line;
    if (row < 0 || cols_ <= 0)
        return line;

    int scrollbackRows = (int)scrollback_.size();
    if (row < scrollbackRows)
    {
        line = scrollback_[(size_t)row];
        if ((int)line.size() < cols_)
            line.append((size_t)(cols_ - line.size()), L' ');
        else if ((int)line.size() > cols_)
            line.resize((size_t)cols_);
        return line;
    }

    if (!screen_)
        return line;

    int screenRow = row - scrollbackRows;
    if (screenRow < 0 || screenRow >= rows_)
        return line;

    line.assign((size_t)cols_, L' ');
    for (int c = 0; c < cols_; ++c)
    {
        VTermPos pos{ screenRow, c };
        VTermScreenCell cell{};
        if (vterm_screen_get_cell(screen_, pos, &cell))
        {
            uint32_t cp = cell.chars[0];
            if (cp == 0) cp = L' ';
            line[(size_t)c] = (wchar_t)cp;
        }
    }
    return line;
}

std::wstring TerminalSession::BuildSelectionText() const
{
    if (!hasSelection_)
        return {};

    int startRow = selectionStartRow_;
    int endRow = selectionEndRow_;
    int startCol = selectionStartCol_;
    int endCol = selectionEndCol_;
    if (startRow > endRow || (startRow == endRow && startCol > endCol))
    {
        std::swap(startRow, endRow);
        std::swap(startCol, endCol);
    }

    startRow = std::max(0, startRow);
    endRow = std::max(0, endRow);

    std::wstring output;
    for (int row = startRow; row <= endRow; ++row)
    {
        std::wstring line = BuildRowText(row);
        int leftCol = 0;
        int rightCol = cols_;
        if (row == startRow)
            leftCol = startCol;
        if (row == endRow)
            rightCol = endCol + 1;

        leftCol = std::max(0, std::min(leftCol, cols_));
        rightCol = std::max(leftCol, std::min(rightCol, cols_));

        std::wstring slice = line.substr((size_t)leftCol, (size_t)(rightCol - leftCol));
        while (!slice.empty() && slice.back() == L' ')
            slice.pop_back();

        if (!output.empty())
            output.append(L"\r\n");
        output.append(slice);
    }

    return output;
}

bool TerminalSession::CopySelectionToClipboard()
{
    if (!hasSelection_ || !hwndOwner_)
        return false;

    std::wstring text = BuildSelectionText();
    if (text.empty())
        return false;

    if (!OpenClipboard(hwndOwner_))
        return false;

    EmptyClipboard();

    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem)
    {
        CloseClipboard();
        return false;
    }

    void* ptr = GlobalLock(hMem);
    if (!ptr)
    {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }

    memcpy(ptr, text.c_str(), bytes);
    GlobalUnlock(hMem);

    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    return true;
}

// ---------------- VTerm ----------------
void TerminalSession::InitVTerm()
{
    DestroyVTerm();

    vt_ = vterm_new(rows_, cols_);
    vterm_set_utf8(vt_, 1);

    screen_ = vterm_obtain_screen(vt_);
    state_ = vterm_obtain_state(vt_);
    vterm_screen_reset(screen_, 1);
    vterm_screen_enable_altscreen(screen_, 1);

    static VTermScreenCallbacks cb{};
    cb.damage = [](VTermRect, void* user) -> int {
        ((TerminalSession*)user)->hasDamage_.store(true);
        return 1;
    };
    cb.movecursor = [](VTermPos, VTermPos, int, void* user) -> int {
        ((TerminalSession*)user)->hasDamage_.store(true);
        return 1;
    };
    cb.settermprop = [](VTermProp, VTermValue*, void* user) -> int {
        ((TerminalSession*)user)->hasDamage_.store(true);
        return 1;
    };
    cb.sb_pushline = [](int cols, const VTermScreenCell* cells, void* user) -> int {
        auto* self = (TerminalSession*)user;

        std::wstring line;
        line.reserve((size_t)cols);
        for (int c = 0; c < cols; ++c)
        {
            uint32_t cp = cells[c].chars[0];
            if (cp == 0) cp = L' ';
            line.push_back((wchar_t)cp);
        }
        while (!line.empty() && line.back() == L' ') line.pop_back();

        self->scrollback_.push_back(std::move(line));

        const size_t MAX_SB = 10000;
        if (self->scrollback_.size() > MAX_SB)
        {
            self->scrollback_.erase(
                self->scrollback_.begin(),
                self->scrollback_.begin() + (self->scrollback_.size() - MAX_SB));
        }
        return 1;
    };
    cb.sb_popline = [](int cols, VTermScreenCell* cells, void* user) -> int {
        auto* self = (TerminalSession*)user;
        if (self->scrollback_.empty()) return 0;

        std::wstring line = std::move(self->scrollback_.back());
        self->scrollback_.pop_back();

        for (int c = 0; c < cols; ++c)
        {
            VTermScreenCell empty{};
            uint32_t cp = (c < (int)line.size()) ? (uint32_t)line[c] : (uint32_t)' ';
            cells[c] = empty;
            cells[c].chars[0] = cp;
            cells[c].width = 1;
        }
        return 1;
    };
    cb.sb_clear = [](void* user) -> int {
        ((TerminalSession*)user)->scrollback_.clear();
        return 1;
    };

    vterm_screen_set_callbacks(screen_, &cb, this);
    vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_SCROLL);

    hasDamage_.store(true);
}

void TerminalSession::DestroyVTerm()
{
    screen_ = nullptr;
    state_ = nullptr;
    if (vt_)
    {
        vterm_free(vt_);
        vt_ = nullptr;
    }
}

void TerminalSession::SetVTermSize(int rows, int cols)
{
    rows_ = rows;
    cols_ = cols;
    if (vt_)
        vterm_set_size(vt_, rows_, cols_);
    hasDamage_.store(true);
}

void TerminalSession::HandleConPTYOutput(const char* data, size_t len)
{
    if (!data || len == 0) return;
    if (!vt_ || !screen_) return;

    vterm_input_write(vt_, data, (int)len);
    vterm_screen_flush_damage(screen_);

    hasDamage_.store(true);

    if (!userScrolling_)
        pendingSnapToBottom_ = true;
}

// ---------------- Scrollbar passthrough ----------------
void TerminalSession::OnMouseWheel(int wheelDelta)
{
    if (scrollbar_.OnMouseWheel(wheelDelta))
        userScrolling_ = true;
}

bool TerminalSession::OnScrollbarMouseMove(POINT pt)
{
    return scrollbar_.OnMouseMove(pt);
}
bool TerminalSession::OnScrollbarLButtonDown(POINT pt)
{
    bool ok = scrollbar_.OnLeftButtonDown(pt);
    if (ok) userScrolling_ = true;
    return ok;
}
bool TerminalSession::OnScrollbarLButtonUp()
{
    return scrollbar_.OnLeftButtonUp();
}

bool TerminalSession::OnLeftButtonDown(POINT pt)
{
    if (pt.x < (LONG)left_ || pt.x > (LONG)right_ || pt.y < (LONG)top_ || pt.y > (LONG)bottom_)
        return false;

    float scrollPx = scrollbar_.GetScrollOffset();
    if (scrollPx < 0) scrollPx = 0;

    int totalRows = (int)scrollback_.size() + rows_;
    if (totalRows <= 0 || cols_ <= 0)
        return false;

    int row = (int)((pt.y - (top_ + padY_) + scrollPx) / lineH_);
    int col = (int)((pt.x - (left_ + padX_)) / charW_);
    row = std::max(0, std::min(row, totalRows - 1));
    col = std::max(0, std::min(col, cols_ - 1));

    selecting_ = true;
    hasSelection_ = true;
    selectionStartRow_ = row;
    selectionStartCol_ = col;
    selectionEndRow_ = row;
    selectionEndCol_ = col;
    return true;
}

bool TerminalSession::OnMouseMove(POINT pt, bool lmbDown)
{
    if (!selecting_ || !lmbDown)
        return false;

    float scrollPx = scrollbar_.GetScrollOffset();
    if (scrollPx < 0) scrollPx = 0;

    int totalRows = (int)scrollback_.size() + rows_;
    if (totalRows <= 0 || cols_ <= 0)
        return false;

    int row = (int)((pt.y - (top_ + padY_) + scrollPx) / lineH_);
    int col = (int)((pt.x - (left_ + padX_)) / charW_);
    row = std::max(0, std::min(row, totalRows - 1));
    col = std::max(0, std::min(col, cols_ - 1));

    if (row == selectionEndRow_ && col == selectionEndCol_)
        return false;

    selectionEndRow_ = row;
    selectionEndCol_ = col;
    return true;
}

bool TerminalSession::OnLeftButtonUp()
{
    if (!selecting_)
        return false;

    selecting_ = false;
    return true;
}

// ---------------- Layout / Draw ----------------
void TerminalSession::SetViewport(float left, float top, float right, float bottom)
{
    left_ = left; top_ = top; right_ = right; bottom_ = bottom;
}

void TerminalSession::UpdatePseudoConsoleSizeFromPixels(float widthPx, float heightPx,
                                                       const std::wstring& fontFamily, float fontSizePx, IDWriteFontCollection* fontCollection)
{
    if (!initialized_ || !pResizePseudoConsole || !hPC_) return;

    if (widthPx < 50.0f || heightPx < 30.0f)
        return;

    // Metrics approximations (comme ton code)
    float charW = fontSizePx * 0.60f;
    float lineH = fontSizePx * 1.35f;

    int cols = (int)(widthPx / std::max(4.0f, charW));
    int rows = (int)(heightPx / std::max(8.0f, lineH));

    cols = std::max(20, cols);
    rows = std::max(5, rows);

    COORD sz{ (SHORT)cols, (SHORT)rows };
    pResizePseudoConsole((HPCON)hPC_, sz);

    SetVTermSize(rows, cols);

    // Invalidate text format cache if font changed (done in DrawContent anyway)
    (void)fontFamily; (void)fontCollection;
}

void TerminalSession::DrawContent(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                                 const std::wstring& fontFamily, float fontSizePx, IDWriteFontCollection* fontCollection,
                                 bool resizeHoverOrResizing, bool isFocused)
{
    if (!rt || !dwrite) return;

    // --- Always draw panel background/chrome here (session content area) ---
    ID2D1SolidColorBrush* bg = nullptr;
    ID2D1SolidColorBrush* fg = nullptr;
    ID2D1SolidColorBrush* border = nullptr;

    HRESULT hr1 = rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.08f, 0.92f), &bg);
    HRESULT hr2 = rt->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.92f, 0.92f, 1.0f), &fg);
    HRESULT hr3 = rt->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.20f, 0.20f, 1.0f), &border);

    if (FAILED(hr1) || FAILED(hr2) || FAILED(hr3) || !bg || !fg || !border)
    {
        if (bg) bg->Release();
        if (fg) fg->Release();
        if (border) border->Release();
        return;
    }

    const D2D1_RECT_F panel = D2D1::RectF(left_, top_, right_, bottom_);
    rt->FillRectangle(panel, bg);
    rt->DrawRectangle(panel, border, 1.0f);

    if (resizeHoverOrResizing)
    {
        ID2D1SolidColorBrush* rz = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.05f), &rz)) && rz)
        {
            rt->FillRectangle(D2D1::RectF(left_, top_, right_, top_ + 10.0f), rz);
            rz->Release();
        }
    }

    // If not ready: stop after chrome (no empty reserved zone)
    if (!vt_ || !screen_)
    {
        bg->Release(); fg->Release(); border->Release();
        return;
    }

    vterm_screen_flush_damage(screen_);

    // TextFormat cache per session (recreate if font changed)
    bool fontChanged = (cachedFontFamily_ != fontFamily) ||
                       (fabs(cachedFontSize_ - fontSizePx) > 0.01f) ||
                       (cachedFontCollection_ != fontCollection);

    if (!textFormat_ || fontChanged)
    {
        if (textFormat_) { textFormat_->Release(); textFormat_ = nullptr; }

        HRESULT hr = dwrite->CreateTextFormat(
            fontFamily.c_str(),
            fontCollection,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSizePx,
            L"en-us",
            &textFormat_);

        if (SUCCEEDED(hr) && textFormat_)
        {
            textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

            cachedFontFamily_ = fontFamily;
            cachedFontSize_ = fontSizePx;
            cachedFontCollection_ = fontCollection;
        }
    }

    if (!textFormat_)
    {
        bg->Release(); fg->Release(); border->Release();
        return;
    }

    // Padding inside the content area
    const float padX = padX_;
    const float padY = padY_;

    const float viewportW = (right_ - left_);
    const float viewportH = (bottom_ - top_);

    charW_ = fontSizePx * 0.60f;
    lineH_ = fontSizePx * 1.35f;

    float contentHeight = padY * 2.0f + (float)((int)scrollback_.size() + rows_) * lineH_;
    scrollbar_.UpdateLayout(left_, top_, viewportW, viewportH, contentHeight);

    if (pendingSnapToBottom_)
    {
        float maxScroll = std::max(0.0f, contentHeight - viewportH);
        scrollbar_.SetScrollOffset(maxScroll);
        pendingSnapToBottom_ = false;
    }

    float maxScroll = std::max(0.0f, contentHeight - viewportH);
    if (scrollbar_.GetScrollOffset() >= maxScroll - 1.0f)
        userScrolling_ = false;

    float scrollPx = scrollbar_.GetScrollOffset();
    if (scrollPx < 0) scrollPx = 0;

    int firstRow = (int)(scrollPx / lineH_);
    float yStart = (top_ + padY) - fmodf(scrollPx, lineH_);

    int totalRows = (int)scrollback_.size() + rows_;
    int maxRowsToDraw = (int)(viewportH / lineH_) + 3;
    int endRow = std::min(totalRows, firstRow + maxRowsToDraw);

    ID2D1SolidColorBrush* selectionBrush = nullptr;
    if (hasSelection_)
        rt->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.45f, 0.85f, 0.45f), &selectionBrush);

    rt->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    for (int r = firstRow; r < endRow; ++r)
    {
        float y = yStart + (float)(r - firstRow) * lineH_;
        if (y > bottom_) break;

        std::wstring line;
        line.reserve((size_t)cols_);

        if (r < (int)scrollback_.size())
        {
            line = scrollback_[(size_t)r];
        }
        else
        {
            int screenRow = r - (int)scrollback_.size();
            for (int c = 0; c < cols_; ++c)
            {
                VTermPos pos{ screenRow, c };
                VTermScreenCell cell{};
                if (vterm_screen_get_cell(screen_, pos, &cell))
                {
                    uint32_t cp = cell.chars[0];
                    if (cp == 0) cp = L' ';
                    line.push_back((wchar_t)cp);
                }
                else
                {
                    line.push_back(L' ');
                }
            }
        }

        if (hasSelection_ && selectionBrush)
        {
            int startRow = selectionStartRow_;
            int endRow = selectionEndRow_;
            int startCol = selectionStartCol_;
            int endCol = selectionEndCol_;
            if (startRow > endRow || (startRow == endRow && startCol > endCol))
            {
                std::swap(startRow, endRow);
                std::swap(startCol, endCol);
            }

            if (r >= startRow && r <= endRow)
            {
                int leftCol = 0;
                int rightCol = cols_;
                if (r == startRow)
                    leftCol = startCol;
                if (r == endRow)
                    rightCol = endCol + 1;

                leftCol = std::max(0, std::min(leftCol, cols_ - 1));
                rightCol = std::max(leftCol + 1, std::min(rightCol, cols_));

                float selLeft = left_ + padX + (float)leftCol * charW_;
                float selRight = left_ + padX + (float)rightCol * charW_;
                D2D1_RECT_F selRect = D2D1::RectF(selLeft, y, selRight, y + lineH_);
                rt->FillRectangle(selRect, selectionBrush);
            }
        }

        while (!line.empty() && line.back() == L' ') line.pop_back();
        if (line.empty()) continue;

        D2D1_RECT_F rect = D2D1::RectF(left_ + padX, y, right_ - 8.0f, y + lineH_);
        rt->DrawTextW(line.c_str(), (UINT32)line.size(), textFormat_, rect, fg, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (isFocused)
    {
        VTermPos cursor{};
        if (state_)
        {
            vterm_state_get_cursorpos(state_, &cursor);
            int cursorRow = (int)scrollback_.size() + cursor.row;
            if (cursorRow >= firstRow && cursorRow < endRow)
            {
                float caretY = yStart + (float)(cursorRow - firstRow) * lineH_;
                float caretX = left_ + padX + (float)cursor.col * charW_;
                if (caretY <= bottom_ && caretX <= right_)
                {
                    D2D1_RECT_F caretRect = D2D1::RectF(
                        caretX,
                        caretY,
                        caretX + std::max(1.0f, charW_ * 0.15f),
                        caretY + lineH_);
                    rt->FillRectangle(caretRect, fg);
                }
            }
        }
    }

    rt->PopAxisAlignedClip();

    scrollbar_.Draw(rt);

    if (selectionBrush)
        selectionBrush->Release();
    bg->Release();
    fg->Release();
    border->Release();
}
