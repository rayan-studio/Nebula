// CustomPopup.cpp — Real OS popup window for context menus, rendered with Direct2D.
// Creates a WS_POPUP HWND (CS_DROPSHADOW) at screen coordinates.
// Hover + selection handled in its own WndProc; sends WM_COMMAND to parent on selection.

#include "CustomPopup.h"
#include "ui/components/menu/DropdownMenu.h"
#include "ui/theme/Theme.h"
#include "utils/logger/Logger.h"

#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <sstream>

static const wchar_t *POPUP_MENU_CLASS = L"NebulaContextMenuV2";
static constexpr int kPopupCornerRadius = 6;

// Layout aligned with the in-window titlebar dropdowns.
static constexpr float kItemH    = kDropdownItemH;
static constexpr float kSepH     = kDropdownSepH;
static constexpr float kInnerPad = kDropdownInnerPad;

// ---------------------------------------------------------------------------
// D2D factories — created once, shared across popups
// ---------------------------------------------------------------------------
static ID2D1Factory   *g_d2d = nullptr;
static IDWriteFactory *g_dw  = nullptr;

static void EnsureFactories()
{
    if (!g_d2d)
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    if (!g_dw)
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(&g_dw));
}

// ---------------------------------------------------------------------------
// Popup state
// ---------------------------------------------------------------------------
struct PopupMenuState
{
    // Content
    std::vector<std::wstring> items;
    std::vector<std::wstring> shortcuts;
    std::vector<bool>         separators;
    std::vector<bool>         enabled;
    int                       baseId   = 0;
    int                       hover    = -1;
    HWND                      parent   = nullptr;
    int                       winW     = 0;
    int                       winH     = 0;

    ID2D1HwndRenderTarget  *rt         = nullptr;

    bool CreateD2D(HWND hwnd)
    {
        if (!g_d2d || !g_dw) return false;

        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE));
        D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
            hwnd, D2D1::SizeU((UINT)winW, (UINT)winH), D2D1_PRESENT_OPTIONS_NONE);
        HRESULT hr = g_d2d->CreateHwndRenderTarget(rtProps, hwndProps, &rt);
        return SUCCEEDED(hr) && rt;
    }

    void ReleaseD2D()
    {
        if (rt)
        {
            rt->Release();
            rt = nullptr;
        }
    }

    ~PopupMenuState() { ReleaseD2D(); }
};

// Global handle to the single active context menu popup
static HWND g_popupHwnd = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int HitTestItem(const PopupMenuState *s, POINT pt)
{
    float y = kInnerPad;
    for (int i = 0; i < (int)s->items.size(); ++i)
    {
        bool hasSep = !s->separators.empty() && i < (int)s->separators.size() && s->separators[i];
        if (hasSep) y += kSepH;
        if (pt.y >= (LONG)y && pt.y < (LONG)(y + kItemH))
        {
            bool isEnabled = s->enabled.empty() || i >= (int)s->enabled.size() || s->enabled[i];
            return isEnabled ? i : -1;
        }
        y += kItemH;
    }
    return -1;
}

static int ComputeHeight(const PopupMenuState *s)
{
    float h = 2.0f * kInnerPad;
    for (int i = 0; i < (int)s->items.size(); ++i)
    {
        bool hasSep = !s->separators.empty() && i < (int)s->separators.size() && s->separators[i];
        if (hasSep) h += kSepH;
        h += kItemH;
    }
    return (int)std::ceil(h);
}

static int ComputeWidth(const PopupMenuState *s)
{
    const bool hasShortcuts = !s->shortcuts.empty() &&
        std::any_of(s->shortcuts.begin(), s->shortcuts.end(),
                    [](const std::wstring &shortcut) { return !shortcut.empty(); });

    size_t maxLabelLen = 0;
    size_t maxShortcutLen = 0;
    for (const auto &item : s->items)
        maxLabelLen = (std::max)(maxLabelLen, item.size());
    for (const auto &shortcut : s->shortcuts)
        maxShortcutLen = (std::max)(maxShortcutLen, shortcut.size());

    float width = 28.0f + (float)maxLabelLen * 7.1f;
    if (hasShortcuts)
        width += 18.0f + (float)maxShortcutLen * 6.3f;

    width = (std::max)(width, hasShortcuts ? 220.0f : 176.0f);
    width = (std::min)(width, hasShortcuts ? 320.0f : 280.0f);
    return (int)std::ceil(width);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void DrawPopup(PopupMenuState *s)
{
    if (!s->rt) return;

    const float W = (float)s->winW;
    const float H = (float)s->winH;
    s->rt->BeginDraw();
    s->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    s->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    MenuDropdown dd = {};
    dd.items = s->items;
    dd.shortcuts = s->shortcuts;
    dd.separators = s->separators;
    dd.enabled = s->enabled;
    dd.hoveredItem = s->hover;
    dd.visible = true;
    dd.rect = D2D1::RectF(0.0f, 0.0f, W, H);
    DrawDropdownPanel(s->rt, g_dw, dd);

    HRESULT hr = s->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        s->ReleaseD2D();
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK PopupMenuWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PopupMenuState *s = reinterpret_cast<PopupMenuState *>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg)
    {
    case WM_CREATE:
    {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        s = reinterpret_cast<PopupMenuState *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        HRGN region = CreateRoundRectRgn(0, 0, s->winW + 1, s->winH + 1,
                                         kPopupCornerRadius * 2, kPopupCornerRadius * 2);
        if (region)
            SetWindowRgn(hwnd, region, FALSE);
        EnsureFactories();
        s->CreateD2D(hwnd);

        // Track mouse leave
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_DESTROY:
        g_popupHwnd = nullptr;
        if (s) delete s;
        return 0;

    case WM_ERASEBKGND:
        return 1;   // Prevent GDI from clearing — D2D paints everything

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;   // Don't steal focus from main window

    case WM_MOUSEMOVE:
    {
        if (!s) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int idx = HitTestItem(s, pt);
        if (idx != s->hover)
        {
            s->hover = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (s && s->hover != -1)
        {
            s->hover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
    {
        if (!s) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int idx = HitTestItem(s, pt);
        if (idx >= 0)
            PostMessageW(s->parent, WM_COMMAND, (WPARAM)(s->baseId + idx), 0);
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_RBUTTONDOWN:
        DestroyWindow(hwnd);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            DestroyWindow(hwnd);
        break;

    case WM_PAINT:
    {
        if (!s) break;
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (s->rt)
            DrawPopup(s);
        else if (s->CreateD2D(hwnd))
            DrawPopup(s);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ShowCustomPopup(HWND parent,
                     const std::vector<std::wstring> &items,
                     POINT screenPos,
                     int baseId,
                     const std::vector<bool> &separators,
                     const std::vector<std::wstring> &shortcuts,
                     const std::vector<bool> &enabled)
{
    CloseCustomPopup();     // Dismiss any currently open popup
    EnsureFactories();

    auto *s      = new PopupMenuState;
    s->items     = items;
    s->shortcuts = shortcuts;
    if (s->shortcuts.size() < items.size()) s->shortcuts.resize(items.size(), L"");
    s->separators = separators;
    if (s->separators.size() < items.size()) s->separators.resize(items.size(), false);
    s->enabled = enabled;
    if (s->enabled.size() < items.size()) s->enabled.resize(items.size(), true);
    s->baseId  = baseId;
    s->hover   = -1;
    s->parent  = parent;
    s->winW    = ComputeWidth(s);
    s->winH    = ComputeHeight(s);

    // Screen edge clipping
    HMONITOR mon = MonitorFromPoint(screenPos, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(mon, &mi);
    RECT work = mi.rcWork;
    if (screenPos.x + s->winW > work.right)  screenPos.x = work.right  - s->winW;
    if (screenPos.y + s->winH > work.bottom) screenPos.y = work.bottom - s->winH;
    if (screenPos.x < work.left) screenPos.x = work.left;
    if (screenPos.y < work.top)  screenPos.y = work.top;

    // Register window class once
    WNDCLASSEXW wc = { sizeof(wc) };
    if (!GetClassInfoExW(GetModuleHandleW(nullptr), POPUP_MENU_CLASS, &wc))
    {
        wc.lpfnWndProc   = PopupMenuWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = POPUP_MENU_CLASS;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.style         = 0;
        RegisterClassExW(&wc);
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        POPUP_MENU_CLASS, L"",
        WS_POPUP,
        screenPos.x, screenPos.y, s->winW, s->winH,
        parent, nullptr, GetModuleHandleW(nullptr), s);

    if (!hwnd) { delete s; return; }

    g_popupHwnd = hwnd;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    Logger::Instance().Log(
        std::wstring(L"CustomPopup: shown at screen (") +
        std::to_wstring(screenPos.x) + L"," + std::to_wstring(screenPos.y) +
        L") size=" + std::to_wstring(s->winW) + L"x" + std::to_wstring(s->winH));
}

void CloseCustomPopup()
{
    if (g_popupHwnd && IsWindow(g_popupHwnd))
    {
        DestroyWindow(g_popupHwnd);
        g_popupHwnd = nullptr;
    }
}

bool IsCustomPopupVisible()
{
    return g_popupHwnd != nullptr && IsWindow(g_popupHwnd);
}

bool IsPointInCustomPopup(POINT screenPt)
{
    if (!g_popupHwnd || !IsWindow(g_popupHwnd)) return false;
    RECT rc;
    GetWindowRect(g_popupHwnd, &rc);
    return PtInRect(&rc, screenPt) != FALSE;
}
