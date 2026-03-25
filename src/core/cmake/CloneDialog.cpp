// CloneDialog.cpp — Modal dialog for cloning a git repository.
// Uses libgit2 (already linked as part of Nebula) for the actual clone.
// Uses the Theme system for colors to match the main application design.

#include "CloneDialog.h"
#include "utils/logger/Logger.h"
#include "ui/theme/Theme.h"
#include "helpers/window_helpers.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <git2.h>
#include <filesystem>
#include <string>
#include <thread>
#include <atomic>
#include <cwctype>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: Convert D2D1_COLOR_F to COLORREF (GDI format)
// ---------------------------------------------------------------------------
static COLORREF D2DColorToCOLORREF(const D2D1_COLOR_F &color)
{
    int r = (int)(color.r * 255.0f);
    int g = (int)(color.g * 255.0f);
    int b = (int)(color.b * 255.0f);
    r = (r < 0) ? 0 : (r > 255) ? 255 : r;
    g = (g < 0) ? 0 : (g > 255) ? 255 : g;
    b = (b < 0) ? 0 : (b > 255) ? 255 : b;
    return RGB(r, g, b);
}

// ---------------------------------------------------------------------------
// Get theme colors dynamically from Theme system
// ---------------------------------------------------------------------------
static COLORREF GetThemeBackground()
{
    return D2DColorToCOLORREF(UI::Theme::ChromeBackground());
}

static COLORREF GetThemeInputBg()
{
    return D2DColorToCOLORREF(UI::Theme::GetPalette().inputBackground);
}

static COLORREF GetThemeBorder()
{
    return D2DColorToCOLORREF(UI::Theme::ChromeBorder());
}

static COLORREF GetThemeFocusBorder()
{
    return D2DColorToCOLORREF(UI::Theme::GetPalette().inputFocusBorder);
}

static COLORREF GetThemeText()
{
    return D2DColorToCOLORREF(UI::Theme::PrimaryText());
}

static COLORREF GetThemeMutedText()
{
    return D2DColorToCOLORREF(UI::Theme::MutedText());
}

static COLORREF GetThemeAccent()
{
    return D2DColorToCOLORREF(UI::Theme::Accent());
}

static COLORREF GetThemeAccentStrong()
{
    return D2DColorToCOLORREF(UI::Theme::AccentStrong());
}

// Fallback error/ok colors (not in main theme)
static COLORREF GetThemeErrorColor()
{
    return RGB(240, 100, 100);  // Soft red for error
}

static COLORREF GetThemeSuccessColor()
{
    return RGB(80, 200, 120);   // Soft green for success
}

// ---------------------------------------------------------------------------
// Helper: Draw rounded rectangle with GDI (for TextInput-like appearance)
// ---------------------------------------------------------------------------
static void DrawRoundedRectGDI(HDC hdc, const RECT &rect, int radius, COLORREF fillColor, COLORREF borderColor)
{
    // Create pen and brush
    HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
    HBRUSH brush = CreateSolidBrush(fillColor);
    
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    
    // Draw rounded rectangle
    int x = rect.left;
    int y = rect.top;
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    int r = radius;
    
    // GDI draws from top-left corner
    RoundRect(hdc, x, y, x + w, y + h, r, r);
    
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
static std::wstring ToWide(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

static std::string ToNarrow(const std::wstring &w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

// "https://github.com/nlohmann/json.git" → "json"
static std::wstring RepoNameFromUrl(const std::wstring &url)
{
    std::wstring u = url;
    if (u.size() >= 4 && u.substr(u.size() - 4) == L".git")
        u = u.substr(0, u.size() - 4);
    while (!u.empty() && (u.back() == L'/' || u.back() == L'\\')) u.pop_back();
    size_t pos = u.find_last_of(L"/\\:");
    if (pos != std::wstring::npos) u = u.substr(pos + 1);
    return u.empty() ? L"library" : u;
}

// ---------------------------------------------------------------------------
// Structure de state du dialog
// ---------------------------------------------------------------------------
struct CloneDlgState
{
    std::wstring destDir;

    HWND hUrlLabel  = nullptr;
    HWND hUrl       = nullptr;
    HWND hStatus    = nullptr;
    HWND hLog       = nullptr;
    HWND hCloneBtn  = nullptr;
    HWND hCancelBtn = nullptr;

    // Custom-drawn progress bar (no Win32 PROGRESS_CLASS control)
    bool  progressVisible = false;
    bool  progressMarquee = false;
    int   progressPos     = 0;      // 0–1000
    float marqueePhase    = 0.f;    // 0–1 normalized position
    float marqueeDir      = 1.f;    // +1 or -1
    RECT  progressRect    = {};     // updated in WM_CREATE / WM_SIZE

    std::wstring clonedPath;
    bool dismissed = false;

    std::atomic<bool> cloning{false};
    std::atomic<bool> cloneOk{false};
    std::atomic<unsigned int> recvObjects{0};
    std::atomic<unsigned int> totalObjects{0};
    std::atomic<unsigned int> indexedObjects{0};
    std::atomic<DWORD> lastProgressTick{0};
    std::wstring cloneError;

    HBRUSH hBgBrush   = nullptr;
    HBRUSH hEditBrush = nullptr;

    // Track status kind for color
    enum class StatusKind { None, Info, Ok, Err } statusKind = StatusKind::None;

    // Cached input bounds for rendering custom border
    RECT inputRect = {0, 0, 0, 0};
    bool inputHasFocus = false;
    // If set, pre-fills the URL and auto-starts the clone
    std::wstring autoStartUrl;

    bool closeBtnHovered = false;
};

static constexpr int TITLE_H         = 35; // logical px — matches main window title bar height
static constexpr UINT MARQUEE_TIMER  = 1;  // WM_TIMER id for progress bar animation

#define WM_CLONE_DONE  (WM_USER + 1)
#define WM_CLONE_PROGRESS (WM_USER + 2)

static void AppendLogLine(CloneDlgState *s, const std::wstring &line)
{
    if (!s || !s->hLog)
        return;

    int len = GetWindowTextLengthW(s->hLog);
    SendMessageW(s->hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring text = line + L"\r\n";
    SendMessageW(s->hLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

static int TransferProgressCallback(const git_indexer_progress *stats, void *payload)
{
    if (!stats || !payload)
        return 0;

    CloneDlgState *s = static_cast<CloneDlgState *>(payload);
    s->recvObjects.store(stats->received_objects);
    s->totalObjects.store(stats->total_objects);
    s->indexedObjects.store(stats->indexed_objects);

    DWORD now = GetTickCount();
    DWORD last = s->lastProgressTick.load();
    if (now - last >= 75)
    {
        s->lastProgressTick.store(now);
        PostMessageW(GetParent(s->hUrl), WM_CLONE_PROGRESS, 0, 0);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Edit subclass — invalidates parent on focus change so the border redraws
// ---------------------------------------------------------------------------
static WNDPROC g_oldEditProc = nullptr;

static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS)
    {
        LRESULT r = CallWindowProcW(g_oldEditProc, hwnd, uMsg, wParam, lParam);
        // Redraw only the border rect around the edit
        RECT rc;
        GetWindowRect(hwnd, &rc);
        HWND parent = GetParent(hwnd);
        MapWindowPoints(nullptr, parent, (POINT *)&rc, 2);
        InflateRect(&rc, 2, 2);
        InvalidateRect(parent, &rc, FALSE);
        return r;
    }
    return CallWindowProcW(g_oldEditProc, hwnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Background clone thread
// ---------------------------------------------------------------------------
static void CloneThread(CloneDlgState *s, std::wstring url, std::wstring destPath, HWND hwnd)
{
    git_libgit2_init();

    git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
    opts.fetch_opts.callbacks.transfer_progress = TransferProgressCallback;
    opts.fetch_opts.callbacks.payload = s;
    git_repository *repo   = nullptr;
    int err = git_clone(&repo, ToNarrow(url).c_str(), ToNarrow(destPath).c_str(), &opts);
    if (repo) git_repository_free(repo);

    if (err != 0)
    {
        const git_error *e = git_error_last();
        s->cloneError = e ? ToWide(e->message) : L"Unknown error";
        s->cloneOk.store(false);
    }
    else
    {
        s->clonedPath = destPath;
        s->cloneOk.store(true);
    }

    git_libgit2_shutdown();
    PostMessageW(hwnd, WM_CLONE_DONE, 0, 0);
}

// ---------------------------------------------------------------------------
// Helper — draw a themed flat button into a DRAWITEMSTRUCT
// ---------------------------------------------------------------------------
static void DrawFlatButton(const DRAWITEMSTRUCT *di, bool isPrimary)
{
    bool pressed  = (di->itemState & ODS_SELECTED) != 0;
    bool disabled = (di->itemState & ODS_DISABLED)  != 0;

    COLORREF bg, fg, border;
    COLORREF buttonBg        = D2DColorToCOLORREF(D2D1::ColorF(52.0f/255, 52.0f/255, 57.0f/255, 1.0f));
    COLORREF buttonBgPressed = D2DColorToCOLORREF(D2D1::ColorF(40.0f/255, 40.0f/255, 45.0f/255, 1.0f));
    
    if (disabled) {
        bg = buttonBg; fg = GetThemeMutedText(); border = GetThemeBorder();
    } else if (isPrimary) {
        bg = pressed ? GetThemeAccentStrong() : GetThemeAccent();
        fg = RGB(255,255,255);
        border = bg;
    } else {
        bg = pressed ? buttonBgPressed : buttonBg;
        fg = GetThemeText();
        border = GetThemeBorder();
    }

    HDC dc = di->hDC;
    RECT rc = di->rcItem;

    // Background
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &rc, br);
    DeleteObject(br);

    // Border (1px)
    HPEN pen   = CreatePen(PS_SOLID, 1, border);
    HPEN oldP  = (HPEN)SelectObject(dc, pen);
    HBRUSH nb  = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH oldB = (HBRUSH)SelectObject(dc, nb);
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, oldP);
    SelectObject(dc, oldB);
    DeleteObject(pen);

    // Text
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    HFONT font = (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0);
    HFONT oldF = font ? (HFONT)SelectObject(dc, font) : nullptr;
    wchar_t text[64] = {};
    GetWindowTextW(di->hwndItem, text, 63);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    if (oldF) SelectObject(dc, oldF);
}

// ---------------------------------------------------------------------------
// Dialog window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK CloneDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CloneDlgState *s = reinterpret_cast<CloneDlgState *>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg)
    {
    // ── Custom frame: strip OS title bar, keep resize borders ───────────────
    case WM_NCCALCSIZE:
    {
        if (!wParam)
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        UINT dpi    = win32_get_dpi_for_window(hwnd);
        int frame_x = win32_get_system_metrics_for_dpi(SM_CXFRAME, dpi);
        int frame_y = win32_get_system_metrics_for_dpi(SM_CYFRAME, dpi);
        int padding = win32_get_system_metrics_for_dpi(SM_CXPADDEDBORDER, dpi);
        NCCALCSIZE_PARAMS *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
        RECT *rc = params->rgrc;
        rc->right  -= frame_x + padding;
        rc->left   += frame_x + padding;
        rc->bottom -= frame_y + padding;
        // top is intentionally not adjusted — our custom title bar lives in the client area
        return 0;
    }

    // ── Hit-testing: caption drag + close button ─────────────────────────────
    case WM_NCHITTEST:
    {
        LRESULT hit = DefWindowProcW(hwnd, uMsg, wParam, lParam);
        // Keep resize handles on all edges
        switch (hit)
        {
        case HTRIGHT: case HTLEFT:
        case HTTOPLEFT: case HTTOP: case HTTOPRIGHT:
        case HTBOTTOMRIGHT: case HTBOTTOM: case HTBOTTOMLEFT:
            return hit;
        }

        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        RECT cr; GetClientRect(hwnd, &cr);

        if (pt.y >= 0 && pt.y < TITLE_H)
        {
            // Close button: rightmost TITLE_H-wide strip of the title bar
            if (pt.x >= cr.right - TITLE_H)
                return HTCLOSE;
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    // ── Track close-button hover for custom painting ─────────────────────────
    case WM_NCMOUSEMOVE:
    {
        if (!s) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        RECT cr; GetClientRect(hwnd, &cr);
        bool hov = (pt.x >= cr.right - TITLE_H && pt.y >= 0 && pt.y < TITLE_H);
        if (hov != s->closeBtnHovered)
        {
            s->closeBtnHovered = hov;
            RECT closeRect = { cr.right - TITLE_H, 0, cr.right, TITLE_H };
            InvalidateRect(hwnd, &closeRect, FALSE);
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    case WM_NCMOUSELEAVE:
    {
        if (s && s->closeBtnHovered)
        {
            s->closeBtnHovered = false;
            RECT cr; GetClientRect(hwnd, &cr);
            RECT closeRect = { cr.right - TITLE_H, 0, cr.right, TITLE_H };
            InvalidateRect(hwnd, &closeRect, FALSE);
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    // ── Marquee animation ────────────────────────────────────────────────────
    case WM_TIMER:
        if (wParam == MARQUEE_TIMER && s && s->progressMarquee)
        {
            s->marqueePhase += s->marqueeDir * 0.025f;
            if (s->marqueePhase >= 1.f) { s->marqueePhase = 1.f; s->marqueeDir = -1.f; }
            if (s->marqueePhase <= 0.f) { s->marqueePhase = 0.f; s->marqueeDir =  1.f; }
            InvalidateRect(hwnd, &s->progressRect, FALSE);
        }
        return 0;

    // ── Construction ────────────────────────────────────────────────────────
    case WM_CREATE:
    {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        s = reinterpret_cast<CloneDlgState *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));

        s->hBgBrush   = CreateSolidBrush(GetThemeBackground());
        s->hEditBrush = CreateSolidBrush(GetThemeInputBg());

        HINSTANCE hi = GetModuleHandleW(nullptr);
        RECT clientRc; GetClientRect(hwnd, &clientRc);
        const int W   = clientRc.right - clientRc.left; // accurate client width after WM_NCCALCSIZE
        const int pad = 20;
        const int btnW = 80, btnH = 28;  // Button dimensions

        // ── Font ──
        HFONT hFont = CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Variable Text");
        if (!hFont)
            hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // ── URL label ──
        s->hUrlLabel = CreateWindowExW(0, L"STATIC", L"Repository URL",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            pad, TITLE_H + 18, W - 2*pad, 16, hwnd, nullptr, hi, nullptr);
        SendMessageW(s->hUrlLabel, WM_SETFONT, (WPARAM)hFont, FALSE);

        // ── URL edit (no OS border — we draw our own) ──
        // Use ES_MULTILINE with vertical centering
        s->hUrl = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            pad + 1, TITLE_H + 40, W - 2*pad - 2, 32, hwnd, nullptr, hi, nullptr);
        SendMessageW(s->hUrl, WM_SETFONT, (WPARAM)hFont, FALSE);
        SendMessageW(s->hUrl, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(12, 12));

        // Subclass to repaint border on focus change
        g_oldEditProc = (WNDPROC)SetWindowLongPtrW(s->hUrl, GWLP_WNDPROC,
                                                    (LONG_PTR)EditSubclassProc);

        // Pre-fill from clipboard if it looks like a URL
        if (OpenClipboard(hwnd))
        {
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData)
            {
                wchar_t *clip = static_cast<wchar_t *>(GlobalLock(hData));
                if (clip)
                {
                    std::wstring text(clip);
                    if (text.find(L"http") == 0 || text.find(L"git@") == 0 ||
                        text.find(L"ssh://") == 0)
                        SetWindowTextW(s->hUrl, text.c_str());
                    GlobalUnlock(hData);
                }
            }
            CloseClipboard();
        }

        // ── Status label ──
        s->hStatus = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            pad, TITLE_H + 80, W - 2*pad, 20, hwnd, nullptr, hi, nullptr);
        SendMessageW(s->hStatus, WM_SETFONT, (WPARAM)hFont, FALSE);

        // Progress bar rect (custom-drawn — 4 px tall, vertically centred in the 18 px slot)
        s->progressRect = { pad, TITLE_H + 111, W - pad, TITLE_H + 115 };

        s->hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            pad, TITLE_H + 128, W - 2*pad, 68, hwnd, nullptr, hi, nullptr);
        SendMessageW(s->hLog, WM_SETFONT, (WPARAM)hFont, FALSE);

        // ── Buttons (owner-drawn, more compact) ──
        int btnY = TITLE_H + 206;
        int gap = 12;
        s->hCloneBtn = CreateWindowExW(0, L"BUTTON", L"Clone",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            W - pad - gap - btnW - btnW, btnY, btnW, btnH,
            hwnd, (HMENU)IDOK, hi, nullptr);
        SendMessageW(s->hCloneBtn, WM_SETFONT, (WPARAM)hFont, FALSE);

        s->hCancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            W - pad - btnW, btnY, btnW, btnH,
            hwnd, (HMENU)IDCANCEL, hi, nullptr);
        SendMessageW(s->hCancelBtn, WM_SETFONT, (WPARAM)hFont, FALSE);

        // Store input rect for rendering custom border
        GetWindowRect(s->hUrl, &s->inputRect);
        MapWindowPoints(nullptr, hwnd, (POINT*)&s->inputRect, 2);
        InflateRect(&s->inputRect, 1, 1);

        if (s->autoStartUrl.empty())
        {
            SetFocus(s->hUrl);
            SendMessageW(s->hUrl, EM_SETSEL, 0, -1);
        }
        else
        {
            // Pre-fill URL and lock the field, then auto-start clone
            SetWindowTextW(s->hUrl, s->autoStartUrl.c_str());
            EnableWindow(s->hUrl, FALSE);
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)s->hCloneBtn);
        }
        return 0;
    }

    // ── Background ──────────────────────────────────────────────────────────
    case WM_ERASEBKGND:
    {
        if (s && s->hBgBrush)
        {
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect((HDC)wParam, &rc, s->hBgBrush);
            return 1;
        }
        break;
    }

    // ── Custom title bar + edit border ──────────────────────────────────────
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);

        // ── Title bar background ──
        RECT titleRect = { cr.left, cr.top, cr.right, TITLE_H };
        HBRUSH titleBrush = CreateSolidBrush(GetThemeBackground());
        FillRect(hdc, &titleRect, titleBrush);
        DeleteObject(titleBrush);

        // ── Title text ──
        {
            wchar_t title[128] = {};
            GetWindowTextW(hwnd, title, 127);
            HFONT hFont = CreateFontW(
                -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI Variable Text");
            if (!hFont) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, GetThemeText());
            RECT textRect = titleRect;
            textRect.left  += 14;
            textRect.right -= TITLE_H; // leave room for close button
            DrawTextW(hdc, title, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
            SelectObject(hdc, oldFont);
            DeleteObject(hFont);
        }

        // ── Close button ──
        {
            RECT closeRect = { cr.right - TITLE_H, 0, cr.right, TITLE_H };
            bool hov = s && s->closeBtnHovered;
            COLORREF closeBg = hov ? RGB(196, 43, 28) : GetThemeBackground();
            HBRUSH closeBrush = CreateSolidBrush(closeBg);
            FillRect(hdc, &closeRect, closeBrush);
            DeleteObject(closeBrush);

            HFONT hFont = CreateFontW(
                -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI Variable Text");
            if (!hFont) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, hov ? RGB(255, 255, 255) : GetThemeMutedText());
            DrawTextW(hdc, L"\u00D7", -1, &closeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFont);
            DeleteObject(hFont);
        }

        // ── Separator line below title bar ──
        {
            HPEN sep = CreatePen(PS_SOLID, 1, GetThemeBorder());
            HPEN old = (HPEN)SelectObject(hdc, sep);
            MoveToEx(hdc, cr.left, TITLE_H - 1, nullptr);
            LineTo(hdc, cr.right, TITLE_H - 1);
            SelectObject(hdc, old);
            DeleteObject(sep);
        }

        // ── Custom progress bar ──
        if (s && s->progressVisible)
        {
            const RECT &pr = s->progressRect;
            // Track
            HBRUSH trackBrush = CreateSolidBrush(RGB(50, 50, 55));
            RoundRect(hdc, pr.left, pr.top, pr.right, pr.bottom, 4, 4);
            // Re-draw with proper fill/border
            HPEN   noPen  = (HPEN)GetStockObject(NULL_PEN);
            HPEN   oldPen = (HPEN)SelectObject(hdc, noPen);
            SelectObject(hdc, trackBrush);
            RoundRect(hdc, pr.left, pr.top, pr.right, pr.bottom, 4, 4);
            SelectObject(hdc, oldPen);
            DeleteObject(trackBrush);

            // Fill / marquee segment
            COLORREF accentColor = GetThemeAccent();
            int trackW = pr.right - pr.left;

            if (s->progressMarquee)
            {
                int segW = trackW * 35 / 100;
                int segX = pr.left + (int)((trackW - segW) * s->marqueePhase);
                HBRUSH fillBrush = CreateSolidBrush(accentColor);
                HPEN   fp = (HPEN)SelectObject(hdc, noPen);
                SelectObject(hdc, fillBrush);
                RoundRect(hdc, segX, pr.top, segX + segW, pr.bottom, 4, 4);
                SelectObject(hdc, fp);
                DeleteObject(fillBrush);
            }
            else if (s->progressPos > 0)
            {
                int fillW = trackW * s->progressPos / 1000;
                if (fillW > 0)
                {
                    HBRUSH fillBrush = CreateSolidBrush(accentColor);
                    HPEN   fp = (HPEN)SelectObject(hdc, noPen);
                    SelectObject(hdc, fillBrush);
                    RoundRect(hdc, pr.left, pr.top, pr.left + fillW, pr.bottom, 4, 4);
                    SelectObject(hdc, fp);
                    DeleteObject(fillBrush);
                }
            }
        }

        // ── URL input border ──
        if (s && s->hUrl)
        {
            RECT er;
            GetWindowRect(s->hUrl, &er);
            MapWindowPoints(nullptr, hwnd, (POINT *)&er, 2);
            bool focused = (GetFocus() == s->hUrl);
            s->inputHasFocus = focused;
            COLORREF borderColor = focused ? GetThemeFocusBorder() : GetThemeBorder();
            InflateRect(&er, 2, 2);
            DrawRoundedRectGDI(hdc, er, 6, GetThemeInputBg(), borderColor);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    // ── Control colors ───────────────────────────────────────────────────────
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc  = (HDC)wParam;
        HWND ctl = (HWND)lParam;
        SetBkColor(hdc, GetThemeBackground());
        if (s && ctl == s->hStatus)
        {
            switch (s->statusKind)
            {
            case CloneDlgState::StatusKind::Ok:  SetTextColor(hdc, GetThemeSuccessColor());  break;
            case CloneDlgState::StatusKind::Err: SetTextColor(hdc, GetThemeErrorColor()); break;
            default:                              SetTextColor(hdc, GetThemeMutedText());    break;
            }
        }
        else
        {
            SetTextColor(hdc, GetThemeMutedText());
        }
        return (LRESULT)(s ? s->hBgBrush : nullptr);
    }

    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, GetThemeInputBg());
        SetTextColor(hdc, GetThemeText());
        return (LRESULT)(s ? s->hEditBrush : nullptr);
    }

    // ── Owner-draw buttons ───────────────────────────────────────────────────
    case WM_DRAWITEM:
    {
        auto *di = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
        if (di->CtlType == ODT_BUTTON)
            DrawFlatButton(di, di->CtlID == IDOK);
        return TRUE;
    }

    // ── Commands ─────────────────────────────────────────────────────────────
    case WM_COMMAND:
    {
        if (!s) break;
        int id = LOWORD(wParam);

        if (id == IDOK && !s->cloning.load())
        {
            wchar_t buf[2048] = {};
            GetWindowTextW(s->hUrl, buf, 2047);
            std::wstring url(buf);
            while (!url.empty() && std::iswspace(url.front())) url.erase(url.begin());
            while (!url.empty() && std::iswspace(url.back()))  url.pop_back();

            if (url.empty())
            {
                s->statusKind = CloneDlgState::StatusKind::Err;
                SetWindowTextW(s->hStatus, L"Please enter a repository URL.");
                AppendLogLine(s, L"Please enter a repository URL.");
                return 0;
            }

            std::wstring repoName = RepoNameFromUrl(url);
            std::wstring destPath = s->destDir + L"\\" + repoName;
            std::replace(destPath.begin(), destPath.end(), L'/', L'\\');

            if (fs::exists(destPath))
            {
                s->statusKind = CloneDlgState::StatusKind::Err;
                SetWindowTextW(s->hStatus, (L"Folder already exists: " + repoName).c_str());
                AppendLogLine(s, L"Folder already exists: " + repoName);
                return 0;
            }

            EnableWindow(s->hCloneBtn, FALSE);
            EnableWindow(s->hUrl,      FALSE);
            s->statusKind = CloneDlgState::StatusKind::Info;
            SetWindowTextW(s->hStatus, (L"Cloning " + repoName + L"\u2026").c_str());
            AppendLogLine(s, L"Starting clone: " + url);
            AppendLogLine(s, L"Destination: " + destPath);
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));

            s->recvObjects.store(0);
            s->totalObjects.store(0);
            s->indexedObjects.store(0);
            s->lastProgressTick.store(0);
            s->progressMarquee = true;
            s->progressVisible   = true;
            s->progressMarquee   = true;
            s->progressPos       = 0;
            s->marqueePhase      = 0.f;
            s->marqueeDir        = 1.f;
            SetTimer(hwnd, MARQUEE_TIMER, 16, nullptr);

            s->cloning.store(true);
            s->cloneOk.store(false);
            s->cloneError.clear();

            std::thread([s, url, destPath, hwnd]() {
                CloneThread(s, url, destPath, hwnd);
            }).detach();
        }
        else if (id == IDCANCEL)
        {
            s->dismissed = true;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_CLONE_PROGRESS:
    {
        if (!s) return 0;

        unsigned int total   = s->totalObjects.load();
        unsigned int recv    = s->recvObjects.load();
        unsigned int indexed = s->indexedObjects.load();

        if (total > 0)
        {
            if (s->progressMarquee)
            {
                KillTimer(hwnd, MARQUEE_TIMER);
                s->progressMarquee = false;
                s->progressMarquee   = false;
            }

            int pos = (int)((recv * 1000ULL) / total);
            if (pos < 0) pos = 0;
            if (pos > 1000) pos = 1000;
            s->progressPos = pos;
            InvalidateRect(hwnd, &s->progressRect, FALSE);

            wchar_t msg[256] = {};
            swprintf_s(msg, L"Receiving objects: %u/%u (indexed: %u)", recv, total, indexed);
            SetWindowTextW(s->hStatus, msg);
        }
        return 0;
    }

    // ── Clone finished ───────────────────────────────────────────────────────
    case WM_CLONE_DONE:
    {
        if (!s) break;
        s->cloning.store(false);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));

        KillTimer(hwnd, MARQUEE_TIMER);
        s->progressMarquee   = false;
        s->progressMarquee = false;

        if (s->cloneOk.load())
        {
            s->statusKind  = CloneDlgState::StatusKind::Ok;
            s->progressPos = 1000;
            InvalidateRect(hwnd, &s->progressRect, FALSE);
            SetWindowTextW(s->hStatus, L"Clone successful.");
            AppendLogLine(s, L"Clone successful.");
            s->dismissed = true;
            Sleep(500);
            DestroyWindow(hwnd);
        }
        else
        {
            s->statusKind  = CloneDlgState::StatusKind::Err;
            s->progressPos = 0;
            s->progressVisible = false;
            InvalidateRect(hwnd, &s->progressRect, FALSE);
            SetWindowTextW(s->hStatus, (L"Error: " + s->cloneError).c_str());
            AppendLogLine(s, L"Error: " + s->cloneError);
            EnableWindow(s->hCloneBtn, TRUE);
            EnableWindow(s->hUrl,      TRUE);
            SetFocus(s->hUrl);
        }
        return 0;
    }

    // ── Focus / border color ─────────────────────────────────────────────────
    case WM_ACTIVATE:
    {
        bool focused = (LOWORD(wParam) != WA_INACTIVE);
        HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
        if (hDwm)
        {
            using DwmSetWindowAttribute_t = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
            auto fn = reinterpret_cast<DwmSetWindowAttribute_t>(GetProcAddress(hDwm, "DwmSetWindowAttribute"));
            if (fn)
            {
                const DWORD DWMWA_BORDER_COLOR = 34;
                COLORREF color = focused ? RGB(61, 143, 242) : RGB(51, 51, 51);
                fn(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
            }
            FreeLibrary(hDwm);
        }
        return 0;
    }

    // ── Responsive layout ────────────────────────────────────────────────────
    case WM_SIZE:
    {
        if (!s) break;
        int W = LOWORD(lParam);
        int H = HIWORD(lParam);
        const int pad = 20, btnW = 80, btnH = 28, gap = 12;

        // Buttons anchor to bottom-right
        int btnY = H - pad - btnH;

        // Log expands vertically between the progress bar and the buttons
        int logTop = TITLE_H + 128;
        int logH   = btnY - logTop - 10;
        if (logH < 20) logH = 20;

        const UINT swpFlags = SWP_NOZORDER | SWP_NOACTIVATE;
        HDWP dwp = BeginDeferWindowPos(6);
        if (s->hUrlLabel)  dwp = DeferWindowPos(dwp, s->hUrlLabel,  nullptr, pad,                         TITLE_H + 18,  W - 2*pad,        16,    swpFlags);
        if (s->hUrl)       dwp = DeferWindowPos(dwp, s->hUrl,       nullptr, pad + 1,                     TITLE_H + 40,  W - 2*pad - 2,    32,    swpFlags);
        if (s->hStatus)    dwp = DeferWindowPos(dwp, s->hStatus,    nullptr, pad,                         TITLE_H + 80,  W - 2*pad,        20,    swpFlags);
        // Update custom progress bar rect (no HWND to move)
        s->progressRect = { pad, TITLE_H + 111, W - pad, TITLE_H + 115 };

        if (s->hLog)       dwp = DeferWindowPos(dwp, s->hLog,       nullptr, pad,                         logTop,        W - 2*pad,        logH,  swpFlags);
        if (s->hCloneBtn)  dwp = DeferWindowPos(dwp, s->hCloneBtn,  nullptr, W - pad - gap - btnW - btnW, btnY,          btnW,             btnH,  swpFlags);
        if (s->hCancelBtn) dwp = DeferWindowPos(dwp, s->hCancelBtn, nullptr, W - pad - btnW,              btnY,          btnW,             btnH,  swpFlags);
        EndDeferWindowPos(dwp);

        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    // ── Cleanup ──────────────────────────────────────────────────────────────
    case WM_DESTROY:
        if (s)
        {
            if (s->hBgBrush)   { DeleteObject(s->hBgBrush);   s->hBgBrush   = nullptr; }
            if (s->hEditBrush) { DeleteObject(s->hEditBrush); s->hEditBrush = nullptr; }
        }
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
std::wstring ShowCloneDialog(HWND parent, const std::wstring &destDir)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    static const wchar_t *CLASS = L"NebulaCloneDialog";

    {
        WNDCLASSEXW wc = { sizeof(wc) };
        if (!GetClassInfoExW(GetModuleHandleW(nullptr), CLASS, &wc))
        {
            wc.lpfnWndProc   = CloneDlgProc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.lpszClassName = CLASS;
            wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            RegisterClassExW(&wc);
        }
    }

    try { fs::create_directories(destDir); } catch (...) {}

    CloneDlgState state;
    state.destDir = destDir;
    std::replace(state.destDir.begin(), state.destDir.end(), L'/', L'\\');

    // Compute window size — clientH includes our custom title bar height
    const int clientW = 440, clientH = 260 + TITLE_H;
    RECT adjRc = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&adjRc, WS_THICKFRAME | WS_SYSMENU | WS_VISIBLE, FALSE, 0);
    const int W = adjRc.right  - adjRc.left;
    const int H = adjRc.bottom - adjRc.top;

    // Centre over parent
    RECT pr = {};
    if (parent) GetWindowRect(parent, &pr);
    else { pr.right = GetSystemMetrics(SM_CXSCREEN); pr.bottom = GetSystemMetrics(SM_CYSCREEN); }
    int x = pr.left + (pr.right  - pr.left - W) / 2;
    int y = pr.top  + (pr.bottom - pr.top  - H) / 2;

    HWND hwnd = CreateWindowExW(
        0,
        CLASS, L"Clone Git Repository",
        WS_THICKFRAME | WS_SYSMENU | WS_VISIBLE,
        x, y, W, H,
        parent, nullptr, GetModuleHandleW(nullptr), &state);

    if (!hwnd) return {};

    // Trigger WM_NCCALCSIZE to apply our custom frame stripping
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Apply DWM styling to hide standard frame (same as main window)
    {
        HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
        if (hDwm)
        {
            using DwmSetWindowAttribute_t = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
            auto pDwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttribute_t>(GetProcAddress(hDwm, "DwmSetWindowAttribute"));
            if (pDwmSetWindowAttribute)
            {
                BOOL useDark = TRUE;
                pDwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark));
                pDwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
                const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
                const int DWMSBT_NONE = 1;
                pDwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &DWMSBT_NONE, sizeof(DWMSBT_NONE));
                
                // Set border color to match theme
                const DWORD DWMWA_BORDER_COLOR = 34;
                COLORREF borderColor = RGB(61, 143, 242);
                pDwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
            }
            FreeLibrary(hDwm);
        }
    }

    // Modal message loop
    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parent) EnableWindow(parent, TRUE);
    if (parent) SetForegroundWindow(parent);

    return state.clonedPath;
}

// ---------------------------------------------------------------------------
// ShowInstallCloneDialog — pre-filled URL variant for Marketplace
// ---------------------------------------------------------------------------
std::wstring ShowInstallCloneDialog(HWND parent, const std::wstring &destDir, const std::wstring &url)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    static const wchar_t *CLASS = L"NebulaCloneDialog";

    {
        WNDCLASSEXW wc = { sizeof(wc) };
        if (!GetClassInfoExW(GetModuleHandleW(nullptr), CLASS, &wc))
        {
            wc.lpfnWndProc   = CloneDlgProc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.lpszClassName = CLASS;
            wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            RegisterClassExW(&wc);
        }
    }

    try { fs::create_directories(destDir); } catch (...) {}

    CloneDlgState state;
    state.destDir      = destDir;
    state.autoStartUrl = url;
    std::replace(state.destDir.begin(), state.destDir.end(), L'/', L'\\');

    const int clientW = 440, clientH = 260 + TITLE_H;
    RECT adjRc = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&adjRc, WS_THICKFRAME | WS_SYSMENU | WS_VISIBLE, FALSE, 0);
    const int W = adjRc.right  - adjRc.left;
    const int H = adjRc.bottom - adjRc.top;

    RECT pr = {};
    if (parent) GetWindowRect(parent, &pr);
    else { pr.right = GetSystemMetrics(SM_CXSCREEN); pr.bottom = GetSystemMetrics(SM_CYSCREEN); }
    int x = pr.left + (pr.right  - pr.left - W) / 2;
    int y = pr.top  + (pr.bottom - pr.top  - H) / 2;

    HWND hwnd = CreateWindowExW(
        0, CLASS, L"Installing Library",
        WS_THICKFRAME | WS_SYSMENU | WS_VISIBLE,
        x, y, W, H,
        parent, nullptr, GetModuleHandleW(nullptr), &state);

    if (!hwnd) return {};

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    {
        HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
        if (hDwm)
        {
            using DwmSetWindowAttribute_t = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
            auto pDwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttribute_t>(
                GetProcAddress(hDwm, "DwmSetWindowAttribute"));
            if (pDwmSetWindowAttribute)
            {
                BOOL useDark = TRUE;
                pDwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark));
                pDwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
                const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
                const int DWMSBT_NONE = 1;
                pDwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &DWMSBT_NONE, sizeof(DWMSBT_NONE));
                const DWORD DWMWA_BORDER_COLOR = 34;
                COLORREF borderColor = RGB(61, 143, 242);
                pDwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
            }
            FreeLibrary(hDwm);
        }
    }

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parent) EnableWindow(parent, TRUE);
    if (parent) SetForegroundWindow(parent);

    return state.clonedPath;
}
