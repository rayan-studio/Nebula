// Dialog.cpp — Generic themed modal dialog.
// Visual style mirrors CloneDialog (GDI-based, uses the Theme system).

#include "Dialog.h"
#include "ui/theme/Theme.h"
#include "helpers/window_helpers.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>

// ---------------------------------------------------------------------------
// Theme helpers (identical pattern to CloneDialog)
// ---------------------------------------------------------------------------
static COLORREF D2DColorToCOLORREF(const D2D1_COLOR_F &c)
{
    int r = (int)(c.r * 255.f); r = r < 0 ? 0 : r > 255 ? 255 : r;
    int g = (int)(c.g * 255.f); g = g < 0 ? 0 : g > 255 ? 255 : g;
    int b = (int)(c.b * 255.f); b = b < 0 ? 0 : b > 255 ? 255 : b;
    return RGB(r, g, b);
}

static void SetDlgBorderColor(HWND hwnd, bool focused)
{
    const DWORD DWMWA_BORDER_COLOR = 34;
    COLORREF color = focused ? RGB(61, 143, 242) : RGB(70, 70, 75);
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
}

static COLORREF ThemeBg()          { return D2DColorToCOLORREF(UI::Theme::ChromeBackground()); }
static COLORREF ThemeBorder()      { return D2DColorToCOLORREF(UI::Theme::ChromeBorder()); }
static COLORREF ThemeText()        { return D2DColorToCOLORREF(UI::Theme::PrimaryText()); }
static COLORREF ThemeMutedText()   { return D2DColorToCOLORREF(UI::Theme::MutedText()); }
static COLORREF ThemeAccent()      { return D2DColorToCOLORREF(UI::Theme::Accent()); }
static COLORREF ThemeAccentStrong(){ return D2DColorToCOLORREF(UI::Theme::AccentStrong()); }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct DlgState
{
    std::wstring       message;
    DialogKind         kind    = DialogKind::Info;
    HWND               hMsg    = nullptr;
    HWND               hOkBtn  = nullptr;
    HWND               hNoBtn  = nullptr;
    HBRUSH             hBgBrush = nullptr;
    bool               closeBtnHovered = false;
    bool               hasYesNo  = false;
    bool               confirmed = false;
};

static constexpr int TITLE_H = 35;

// ---------------------------------------------------------------------------
// Button drawing (owner-draw, identical to CloneDialog)
// ---------------------------------------------------------------------------
static void DrawFlatButton(const DRAWITEMSTRUCT *di, bool isPrimary)
{
    bool pressed  = (di->itemState & ODS_SELECTED) != 0;

    static const COLORREF kBtnBg        = RGB(52, 52, 57);
    static const COLORREF kBtnBgPressed = RGB(40, 40, 45);

    COLORREF bg, fg, border;
    if (isPrimary) {
        bg     = pressed ? ThemeAccentStrong() : ThemeAccent();
        fg     = RGB(255, 255, 255);
        border = bg;
    } else {
        bg     = pressed ? kBtnBgPressed : kBtnBg;
        fg     = ThemeText();
        border = ThemeBorder();
    }

    HDC  dc = di->hDC;
    RECT rc = di->rcItem;

    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &rc, br);
    DeleteObject(br);

    HPEN  pen  = CreatePen(PS_SOLID, 1, border);
    HPEN  oldP = (HPEN)SelectObject(dc, pen);
    HBRUSH nb  = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH oldB = (HBRUSH)SelectObject(dc, nb);
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, oldP);
    SelectObject(dc, oldB);
    DeleteObject(pen);

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
// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK DlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    DlgState *s = reinterpret_cast<DlgState *>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg)
    {
    // ── Remove OS title bar ─────────────────────────────────────────────────
    case WM_NCCALCSIZE:
    {
        if (!wParam) return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        UINT dpi    = win32_get_dpi_for_window(hwnd);
        int  frame_x = win32_get_system_metrics_for_dpi(SM_CXFRAME, dpi);
        int  frame_y = win32_get_system_metrics_for_dpi(SM_CYFRAME, dpi);
        int  padding = win32_get_system_metrics_for_dpi(SM_CXPADDEDBORDER, dpi);
        auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
        RECT *rc = params->rgrc;
        rc->right  -= frame_x + padding;
        rc->left   += frame_x + padding;
        rc->bottom -= frame_y + padding;
        return 0;
    }

    // ── Hit-test: caption drag + close button ────────────────────────────────
    case WM_NCHITTEST:
    {
        LRESULT hit = DefWindowProcW(hwnd, uMsg, wParam, lParam);
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
            if (pt.x >= cr.right - TITLE_H) return HTCLOSE;
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    // ── Close button hover ───────────────────────────────────────────────────
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

    // ── Focus / border color ─────────────────────────────────────────────────
    case WM_ACTIVATE:
        SetDlgBorderColor(hwnd, LOWORD(wParam) != WA_INACTIVE);
        return 0;

    // ── Construction ─────────────────────────────────────────────────────────
    case WM_CREATE:
    {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        s = reinterpret_cast<DlgState *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));

        s->hBgBrush = CreateSolidBrush(ThemeBg());

        HINSTANCE hi = GetModuleHandleW(nullptr);
        RECT cr; GetClientRect(hwnd, &cr);
        const int W   = cr.right;
        const int pad = 20;
        const int btnW = 80, btnH = 28;

        HFONT hFont = CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Variable Text");
        if (!hFont) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // Message static control
        s->hMsg = CreateWindowExW(0, L"STATIC", s->message.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            pad, TITLE_H + 18, W - 2 * pad, cr.bottom - TITLE_H - 18 - btnH - 28,
            hwnd, nullptr, hi, nullptr);
        SendMessageW(s->hMsg, WM_SETFONT, (WPARAM)hFont, FALSE);

        if (s->hasYesNo)
        {
            const int gap = 8;
            // "No" button (secondary, right)
            s->hNoBtn = CreateWindowExW(0, L"BUTTON", L"No",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                W - pad - btnW, cr.bottom - pad - btnH, btnW, btnH,
                hwnd, (HMENU)IDNO, hi, nullptr);
            SendMessageW(s->hNoBtn, WM_SETFONT, (WPARAM)hFont, FALSE);

            // "Yes" button (primary, left of No)
            s->hOkBtn = CreateWindowExW(0, L"BUTTON", L"Yes",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                W - pad - gap - 2 * btnW, cr.bottom - pad - btnH, btnW, btnH,
                hwnd, (HMENU)IDYES, hi, nullptr);
            SendMessageW(s->hOkBtn, WM_SETFONT, (WPARAM)hFont, FALSE);
        }
        else
        {
            // OK button
            s->hOkBtn = CreateWindowExW(0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                W - pad - btnW, cr.bottom - pad - btnH, btnW, btnH,
                hwnd, (HMENU)IDOK, hi, nullptr);
            SendMessageW(s->hOkBtn, WM_SETFONT, (WPARAM)hFont, FALSE);
        }

        SetFocus(s->hOkBtn);
        return 0;
    }

    case WM_DESTROY:
        if (s)
        {
            if (s->hBgBrush) DeleteObject(s->hBgBrush);
        }
        return 0;

    // ── Background ───────────────────────────────────────────────────────────
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

    // ── Custom title bar ─────────────────────────────────────────────────────
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);

        // Title bar background
        RECT titleRect = { cr.left, cr.top, cr.right, TITLE_H };
        HBRUSH titleBrush = CreateSolidBrush(ThemeBg());
        FillRect(hdc, &titleRect, titleBrush);
        DeleteObject(titleBrush);

        // Title text
        {
            wchar_t title[128] = {};
            GetWindowTextW(hwnd, title, 127);
            HFONT hFont = CreateFontW(
                -13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI Variable Text");
            if (!hFont) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, ThemeText());
            RECT textRect = titleRect;
            textRect.left  += 12;
            textRect.right -= TITLE_H;
            DrawTextW(hdc, title, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
            SelectObject(hdc, oldFont);
            DeleteObject(hFont);
        }

        // Close button
        {
            RECT closeRect = { cr.right - TITLE_H, 0, cr.right, TITLE_H };
            bool hov = s && s->closeBtnHovered;
            COLORREF closeBg = hov ? RGB(196, 43, 28) : ThemeBg();
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
            SetTextColor(hdc, hov ? RGB(255, 255, 255) : ThemeMutedText());
            DrawTextW(hdc, L"\u00D7", -1, &closeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFont);
            DeleteObject(hFont);
        }

        // Separator line below title bar
        {
            HPEN sep = CreatePen(PS_SOLID, 1, ThemeBorder());
            HPEN old = (HPEN)SelectObject(hdc, sep);
            MoveToEx(hdc, cr.left, TITLE_H - 1, nullptr);
            LineTo(hdc, cr.right, TITLE_H - 1);
            SelectObject(hdc, old);
            DeleteObject(sep);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    // ── Static control colors ────────────────────────────────────────────────
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, ThemeBg());
        SetTextColor(hdc, ThemeText());
        if (s && s->hBgBrush) return (LRESULT)s->hBgBrush;
        break;
    }

    // ── Owner-draw button ────────────────────────────────────────────────────
    case WM_DRAWITEM:
    {
        const auto *di = reinterpret_cast<const DRAWITEMSTRUCT *>(lParam);
        if (di && (di->CtlID == IDOK || di->CtlID == IDYES))
        {
            DrawFlatButton(di, true);
            return TRUE;
        }
        if (di && di->CtlID == IDNO)
        {
            DrawFlatButton(di, false);
            return TRUE;
        }
        break;
    }

    // ── Commands ─────────────────────────────────────────────────────────────
    case WM_COMMAND:
    {
        WORD id = LOWORD(wParam);
        if (id == IDYES)
        {
            if (s) s->confirmed = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDOK || id == IDCANCEL || id == IDNO)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Shared class registration
// ---------------------------------------------------------------------------
static constexpr wchar_t kDlgClassName[] = L"NebulaThemedDialog";

static void EnsureClassRegistered()
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = DlgProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kDlgClassName;
        RegisterClassExW(&wc);
        registered = true;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ShowDialog(HWND parent,
                const std::wstring &title,
                const std::wstring &message,
                DialogKind          kind)
{
    EnsureClassRegistered();

    // Compute client size then adjust for the window frame (same technique as CloneDialog)
    const int clientW = 460, clientH = 220;
    RECT adjRc = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&adjRc, WS_THICKFRAME | WS_SYSMENU, FALSE, 0);
    const int W = adjRc.right  - adjRc.left;
    const int H = adjRc.bottom - adjRc.top;

    int x = 0, y = 0;
    if (parent)
    {
        RECT pr; GetWindowRect(parent, &pr);
        x = pr.left + (pr.right  - pr.left - W) / 2;
        y = pr.top  + (pr.bottom - pr.top  - H) / 2;
    }

    auto *s = new DlgState();
    s->message = message;
    s->kind    = kind;

    HWND hwnd = CreateWindowExW(
        0,
        kDlgClassName,
        title.c_str(),
        WS_THICKFRAME | WS_SYSMENU | WS_VISIBLE,
        x, y, W, H,
        parent, nullptr, GetModuleHandleW(nullptr), s);

    if (!hwnd) { delete s; return; }

    // Force WM_NCCALCSIZE to strip the OS title bar (same as CloneDialog)
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    // DWM: dark mode (border color is handled dynamically via WM_ACTIVATE)
    {
        BOOL useDark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark));
        DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
        const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
        const int   DWMSBT_NONE = 1;
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &DWMSBT_NONE, sizeof(DWMSBT_NONE));
    }

    UpdateWindow(hwnd);

    // Disable parent to make this modal
    if (parent) EnableWindow(parent, FALSE);

    // Standard modal message loop
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
}

// ---------------------------------------------------------------------------
// ShowConfirmDialog — Yes / No variant
// ---------------------------------------------------------------------------
bool ShowConfirmDialog(HWND parent,
                       const std::wstring &title,
                       const std::wstring &message,
                       DialogKind          kind)
{
    EnsureClassRegistered();

    const int clientW = 460, clientH = 220;
    RECT adjRc = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&adjRc, WS_THICKFRAME | WS_SYSMENU, FALSE, 0);
    const int W = adjRc.right  - adjRc.left;
    const int H = adjRc.bottom - adjRc.top;

    int x = 0, y = 0;
    if (parent)
    {
        RECT pr; GetWindowRect(parent, &pr);
        x = pr.left + (pr.right  - pr.left - W) / 2;
        y = pr.top  + (pr.bottom - pr.top  - H) / 2;
    }

    auto *s = new DlgState();
    s->message  = message;
    s->kind     = kind;
    s->hasYesNo = true;

    HWND hwnd = CreateWindowExW(
        0,
        kDlgClassName,
        title.c_str(),
        WS_THICKFRAME | WS_SYSMENU | WS_VISIBLE,
        x, y, W, H,
        parent, nullptr, GetModuleHandleW(nullptr), s);

    if (!hwnd) { delete s; return false; }

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    {
        HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
        if (hDwm)
        {
            using Fn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
            auto pDwm = reinterpret_cast<Fn>(GetProcAddress(hDwm, "DwmSetWindowAttribute"));
            if (pDwm)
            {
                BOOL useDark = TRUE;
                pDwm(hwnd, 20, &useDark, sizeof(useDark));
                pDwm(hwnd, 19, &useDark, sizeof(useDark));
                const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
                const int   DWMSBT_NONE = 1;
                pDwm(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &DWMSBT_NONE, sizeof(DWMSBT_NONE));
                const DWORD DWMWA_BORDER_COLOR = 34;
                COLORREF borderColor = D2DColorToCOLORREF(UI::Theme::Accent());
                pDwm(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
            }
            FreeLibrary(hDwm);
        }
    }

    UpdateWindow(hwnd);

    if (parent) EnableWindow(parent, FALSE);

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

    bool result = s->confirmed;
    delete s;
    return result;
}
