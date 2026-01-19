#include "TerminalPanel.h"

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

// ---------------------------------------------
// libvterm include UNIQUEMENT ici (cpp)
// + macro push/pop pour Windows
// ---------------------------------------------
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

static bool LoadConPTY()
{
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel) return false;

    pCreatePseudoConsole = (CreatePseudoConsole_t)GetProcAddress(hKernel, "CreatePseudoConsole");
    pResizePseudoConsole = (ResizePseudoConsole_t)GetProcAddress(hKernel, "ResizePseudoConsole");
    pClosePseudoConsole = (ClosePseudoConsole_t)GetProcAddress(hKernel, "ClosePseudoConsole");

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

// ---------------- VTerm callbacks (cachés ici) ----------------
static int VTermDamageCb(VTermRect, void* user)
{
    auto* self = (TerminalPanel*)user;
    self->HandleConPTYOutput(nullptr, 0); // no-op safety
    return 1;
}
static int VTermMoveCursorCb(VTermPos, VTermPos, int, void* user)
{
    auto* self = (TerminalPanel*)user;
    (void)self;
    return 1;
}
static int VTermSetTermPropCb(VTermProp, VTermValue*, void* user)
{
    auto* self = (TerminalPanel*)user;
    (void)self;
    return 1;
}
static int VTermSbPushlineCb(int cols, const VTermScreenCell* cells, void* user)
{
    auto* self = (TerminalPanel*)user;

    std::wstring line;
    line.reserve((size_t)cols);
    for (int c = 0; c < cols; ++c)
    {
        uint32_t cp = cells[c].chars[0];
        if (cp == 0) cp = L' ';
        line.push_back((wchar_t)cp);
    }
    while (!line.empty() && line.back() == L' ') line.pop_back();

    // ⚠️ on ne peut pas accéder à scrollback_ (private) ici
    // Donc: on va passer par une méthode (plus bas).
    // Mais pour rester simple, on va déclarer ces callbacks dans la classe normalement.
    // -> Du coup: on va pas utiliser ces callbacks free ici.
    // (NOTE: on va revenir à des callbacks membres, mais dans ce fichier uniquement.)
    return 1;
}

// -----------------------------------------------------------
// Global accessor
// -----------------------------------------------------------
TerminalPanel& GetTerminalPanel()
{
    static TerminalPanel gTerminal;
    return gTerminal;
}

TerminalPanel::TerminalPanel()
{
    fontFamily_ = L"JetBrains Mono";
    fontSize_ = 13.0f;
    fontCollection_ = nullptr;
}

TerminalPanel::~TerminalPanel()
{
    Shutdown();
}

// ---------------- Visible / Focus expected by Window.cpp ----------------
void TerminalPanel::ToggleVisible()
{
    visible_ = !visible_;
    if (!visible_)
        focused_ = false;
}

void TerminalPanel::SetVisible(bool v)
{
    visible_ = v;
    if (!visible_)
        focused_ = false;
}

void TerminalPanel::Unfocus()
{
    focused_ = false;
}

// ---------------- Font control ----------------
void TerminalPanel::SetFont(const std::wstring& family, float sizePx)
{
    fontFamily_ = family;
    fontSize_ = sizePx;

    if (textFormat_)
    {
        textFormat_->Release();
        textFormat_ = nullptr;
    }

    if (initialized_)
        UpdatePseudoConsoleSizeFromPixels();
}

void TerminalPanel::SetFontCollection(IDWriteFontCollection* fc)
{
    fontCollection_ = fc;

    if (textFormat_)
    {
        textFormat_->Release();
        textFormat_ = nullptr;
    }

    if (initialized_)
        UpdatePseudoConsoleSizeFromPixels();
}

// --------- Legacy wrapper ----------
void TerminalPanel::Draw(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, HWND hwnd)
{
    (void)hwnd;
    Draw(rt, dwrite);
}

// ---------------- Layout ----------------
void TerminalPanel::UpdateLayout(HWND hwnd, float left, float top, float right, float bottom)
{
    (void)hwnd;

    left_ = left; top_ = top; right_ = right; bottom_ = bottom;

    state_.leftEdge = left;
    state_.topEdge = top;
    state_.rightEdge = right;
    state_.bottomEdge = bottom;
    state_.physicalWidth = (int)std::max(0.0f, right - left);

    if (initialized_)
        UpdatePseudoConsoleSizeFromPixels();
}

bool TerminalPanel::IsPointInPanel(POINT pt) const
{
    if (!visible_) return false;
    return pt.x >= (LONG)left_ && pt.x <= (LONG)right_
        && pt.y >= (LONG)top_ && pt.y <= (LONG)bottom_;
}

bool TerminalPanel::IsPointInResizeZone(POINT pt) const
{
    if (!visible_) return false;
    return pt.x >= (LONG)left_ && pt.x <= (LONG)right_
        && pt.y >= (LONG)top_ && pt.y <= (LONG)(top_ + resizeZoneH_);
}

// ---------------- Mouse ----------------
void TerminalPanel::OnLeftButtonDown(HWND hwnd, POINT pt)
{
    if (!visible_) return;

    if (scrollbar_.OnLeftButtonDown(pt))
    {
        SetCapture(hwnd);
        userScrolling_ = true;
        return;
    }

    if (IsPointInResizeZone(pt))
    {
        resizing_ = true;
        dragStart_ = pt;
        startTop_ = top_;
        SetCapture(hwnd);
        return;
    }

    if (IsPointInPanel(pt))
        focused_ = true;
}

void TerminalPanel::OnLeftButtonUp(HWND hwnd)
{
    (void)hwnd;

    if (scrollbar_.OnLeftButtonUp())
    {
        ReleaseCapture();
        return;
    }

    if (resizing_)
    {
        resizing_ = false;
        ReleaseCapture();
        return;
    }
}

bool TerminalPanel::OnMouseMove(HWND hwnd, POINT pt)
{
    (void)hwnd;
    if (!visible_) return false;

    if (scrollbar_.OnMouseMove(pt))
        return true;

    if (resizing_)
    {
        int dy = (pt.y - dragStart_.y);
        float newTop = startTop_ + (float)dy;

        float maxTop = bottom_ - minHeight_;
        newTop = ClampF(newTop, 0.0f, maxTop);

        if (newTop != top_)
        {
            top_ = newTop;
            if (initialized_)
                UpdatePseudoConsoleSizeFromPixels();
            return true;
        }
        return false;
    }

    bool prev = resizeHover_;
    resizeHover_ = IsPointInResizeZone(pt);
    return (prev != resizeHover_);
}

void TerminalPanel::OnMouseWheel(HWND hwnd, int wheelDelta)
{
    (void)hwnd;
    if (!visible_) return;

    if (scrollbar_.OnMouseWheel(wheelDelta))
        userScrolling_ = true;
}

// ---------------- Keyboard -> ConPTY ----------------
void TerminalPanel::WriteUtf8(const char* bytes, DWORD len)
{
    if (!initialized_ || !hInW_ || !bytes || len == 0) return;
    DWORD written = 0;
    WriteFile(hInW_, bytes, len, &written, NULL);
}

void TerminalPanel::WriteVtSequence(const char* seq)
{
    if (!seq) return;
    WriteUtf8(seq, (DWORD)strlen(seq));
}

void TerminalPanel::OnChar(wchar_t ch)
{
    if (!visible_ || !focused_ || !initialized_) return;

    if (ch == L'\r' || ch == L'\n' || ch == L'\b' || ch == 0x1B || ch == L'\t')
        return;

    std::string u8 = WideToUtf8Char(ch);
    if (!u8.empty())
        WriteUtf8(u8.data(), (DWORD)u8.size());
}

void TerminalPanel::OnKeyDown(WPARAM vk)
{
    if (!visible_ || !focused_ || !initialized_) return;

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

// ---------------- VTerm init/shutdown (hidden types here) ----------------
void TerminalPanel::InitVTerm()
{
    DestroyVTerm();

    VTerm* vt = vterm_new(rows_, cols_);
    vterm_set_utf8(vt, 1);

    VTermScreen* screen = vterm_obtain_screen(vt);
    vterm_screen_reset(screen, 1);
    vterm_screen_enable_altscreen(screen, 1);

    static VTermScreenCallbacks cb{};
    cb.damage = [](VTermRect, void* user) -> int {
        ((TerminalPanel*)user)->hasDamage_.store(true);
        return 1;
    };
    cb.movecursor = [](VTermPos, VTermPos, int, void* user) -> int {
        ((TerminalPanel*)user)->hasDamage_.store(true);
        return 1;
    };
    cb.settermprop = [](VTermProp, VTermValue*, void* user) -> int {
        ((TerminalPanel*)user)->hasDamage_.store(true);
        return 1;
    };
    cb.sb_pushline = [](int cols, const VTermScreenCell* cells, void* user) -> int {
        auto* self = (TerminalPanel*)user;

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
        auto* self = (TerminalPanel*)user;
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
        ((TerminalPanel*)user)->scrollback_.clear();
        return 1;
    };

    vterm_screen_set_callbacks(screen, &cb, this);
    vterm_screen_set_damage_merge(screen, VTERM_DAMAGE_SCROLL);

    vt_ = vt;
    screen_ = screen;

    hasDamage_.store(true);
}

void TerminalPanel::DestroyVTerm()
{
    screen_ = nullptr;
    if (vt_)
    {
        vterm_free((VTerm*)vt_);
        vt_ = nullptr;
    }
}

void TerminalPanel::SetVTermSize(int rows, int cols)
{
    rows_ = rows;
    cols_ = cols;
    if (vt_)
        vterm_set_size((VTerm*)vt_, rows_, cols_);
    hasDamage_.store(true);
}

// ---------------- Output ConPTY -> VTerm ----------------
void TerminalPanel::HandleConPTYOutput(const char* data, size_t len)
{
    if (!data || len == 0) return;
    if (!vt_ || !screen_) return;

    vterm_input_write((VTerm*)vt_, data, (int)len);
    vterm_screen_flush_damage((VTermScreen*)screen_);

    hasDamage_.store(true);

    if (!userScrolling_)
        pendingSnapToBottom_ = true;
}

// ---------------- ConPTY init/shutdown ----------------
bool TerminalPanel::Initialize(HWND hwnd, const std::wstring& startDir)
{
    if (initialized_) return true;

    conptyLoaded_ = LoadConPTY();
    if (!conptyLoaded_) return false;

    hwndOwner_ = hwnd;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&hInR_, &hInW_, &sa, 0)) return false;
    if (!CreatePipe(&hOutR_, &hOutW_, &sa, 0)) return false;

    COORD sz{ (SHORT)cols_, (SHORT)rows_ };
    HRESULT hr = pCreatePseudoConsole(sz, hInR_, hOutW_, 0, (HPCON*)&hPC_);
    if (FAILED(hr) || !hPC_) return false;

    if (!StartShellProcess(startDir)) return false;

    initialized_ = true;

    InitVTerm();
    StartReadThread(hwnd);

    return true;
}

bool TerminalPanel::StartShellProcess(const std::wstring& startDir)
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

void TerminalPanel::StartReadThread(HWND hwnd)
{
    hwndOwner_ = hwnd;
    stopThread_ = false;

    if (hReadThread_)
    {
        CloseHandle(hReadThread_);
        hReadThread_ = NULL;
    }

    hReadThread_ = CreateThread(NULL, 0, ReadThreadProc, this, 0, NULL);
}

DWORD WINAPI TerminalPanel::ReadThreadProc(LPVOID p)
{
    auto* self = (TerminalPanel*)p;
    if (!self) return 0;

    const DWORD BUF_SZ = 4096;

    while (!self->stopThread_.load())
    {
        char* buf = (char*)malloc(BUF_SZ);
        if (!buf) break;

        DWORD read = 0;
        BOOL ok = ReadFile(self->hOutR_, buf, BUF_SZ, &read, NULL);

        if (!ok || read == 0)
        {
            free(buf);
            Sleep(1);
            continue;
        }

        if (self->hwndOwner_)
        {
            PostMessageW(self->hwndOwner_, WM_USER + 201, (WPARAM)read, (LPARAM)buf);
        }
        else
        {
            self->HandleConPTYOutput(buf, read);
            free(buf);
        }
    }

    return 0;
}

void TerminalPanel::UpdatePseudoConsoleSizeFromPixels()
{
    if (!initialized_ || !pResizePseudoConsole || !hPC_) return;

    // ✅ Guard : pas de resize tant qu’on n’a pas un layout valide
    if ((right_ - left_) < 50.0f || (bottom_ - top_) < 30.0f)
        return;

    const float padX = 10.0f;
    const float padY = 8.0f;

    float wPx = (right_ - left_) - padX * 2.0f;
    float hPx = (bottom_ - top_) - padY * 2.0f;

    float charW = fontSize_ * 0.60f;
    float lineH = fontSize_ * 1.35f;

    int cols = (int)(wPx / std::max(4.0f, charW));
    int rows = (int)(hPx / std::max(8.0f, lineH));

    cols = std::max(20, cols);
    rows = std::max(5, rows);

    COORD sz{ (SHORT)cols, (SHORT)rows };
    pResizePseudoConsole((HPCON)hPC_, sz);

    SetVTermSize(rows, cols);
}

void TerminalPanel::Shutdown()
{
    stopThread_ = true;
    if (hReadThread_)
    {
        WaitForSingleObject(hReadThread_, 200);
        CloseHandle(hReadThread_);
        hReadThread_ = NULL;
    }

    CloseConPTY();

    if (textFormat_)
    {
        textFormat_->Release();
        textFormat_ = nullptr;
    }

    DestroyVTerm();
    initialized_ = false;
}

void TerminalPanel::CloseConPTY()
{
    if (hChild_) { CloseHandle(hChild_); hChild_ = NULL; }

    if (hPC_ && pClosePseudoConsole)
    {
        pClosePseudoConsole((HPCON)hPC_);
        hPC_ = NULL;
    }

    if (hInR_) { CloseHandle(hInR_); hInR_ = NULL; }
    if (hInW_) { CloseHandle(hInW_); hInW_ = NULL; }
    if (hOutR_) { CloseHandle(hOutR_); hOutR_ = NULL; }
    if (hOutW_) { CloseHandle(hOutW_); hOutW_ = NULL; }
}

// ---------------- Render (VTerm grid) ----------------
void TerminalPanel::Draw(ID2D1RenderTarget* rt, IDWriteFactory* dwrite)
{
    if (!visible_ || !rt || !dwrite) return;

    // Brushes (create early so we can always draw chrome even if terminal not ready)
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
    rt->FillRectangle(D2D1::RectF(left_, top_, right_, top_ + 1.0f), border);

    if (resizeHover_ || resizing_)
    {
        ID2D1SolidColorBrush* rz = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.06f), &rz)) && rz)
        {
            rt->FillRectangle(D2D1::RectF(left_, top_, right_, top_ + resizeZoneH_), rz);
            rz->Release();
        }
    }

    // If terminal internals are not ready, we stop after drawing the chrome
    if (!vt_ || !screen_)
    {
        // Optionally draw a hint here using a lightweight format (skipped for brevity)
        bg->Release();
        fg->Release();
        border->Release();
        return;
    }

    // Now safe to flush vterm damage
    vterm_screen_flush_damage((VTermScreen*)screen_);

    // Text format (create only when terminal ready)
    if (!textFormat_)
    {
        HRESULT hr = dwrite->CreateTextFormat(
            fontFamily_.c_str(),
            fontCollection_,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSize_,
            L"en-us",
            &textFormat_);

        if (FAILED(hr) || !textFormat_)
        {
            bg->Release();
            fg->Release();
            border->Release();
            return;
        }

        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    const float padX = 10.0f;
    const float padY = 8.0f;

    const float viewportW = (right_ - left_);
    const float viewportH = (bottom_ - top_);

    charW_ = fontSize_ * 0.60f;
    lineH_ = fontSize_ * 1.35f;

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
                if (vterm_screen_get_cell((VTermScreen*)screen_, pos, &cell))
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

        while (!line.empty() && line.back() == L' ') line.pop_back();
        if (line.empty()) continue;

        D2D1_RECT_F rect = D2D1::RectF(left_ + padX, y, right_ - 8.0f, y + lineH_);
        rt->DrawTextW(line.c_str(), (UINT32)line.size(), textFormat_, rect, fg, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    rt->PopAxisAlignedClip();

    scrollbar_.Draw(rt);

    bg->Release();
    fg->Release();
    border->Release();
}
