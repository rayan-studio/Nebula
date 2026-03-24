// CustomPopup.cpp — Real OS popup window for context menus, rendered with Direct2D.
// Creates a WS_POPUP HWND (CS_DROPSHADOW) at screen coordinates.
// Hover + selection handled in its own WndProc; sends WM_COMMAND to parent on selection.

#include "CustomPopup.h"
#include "ui/theme/Theme.h"
#include "utils/logger/Logger.h"

#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <sstream>

static const wchar_t *POPUP_MENU_CLASS = L"NebulaContextMenuV2";

// Layout (mirror DropdownMenu constants)
static constexpr float kItemH    = 32.0f;
static constexpr float kSepH     = 10.0f;
static constexpr float kInnerPad =  5.0f;

// ---------------------------------------------------------------------------
// Icon glyph mapping — Segoe MDL2 Assets codepoints
// ---------------------------------------------------------------------------
static std::wstring ContextIconGlyph(const std::wstring &label)
{
    if (label.find(L"Ouvrir le dossier")  != std::wstring::npos) return L"\uE8DA";
    if (label.find(L"Ouvrir le fichier")  != std::wstring::npos) return L"\uE7C3";
    if (label.find(L"Ouvrir dans")        != std::wstring::npos) return L"\uEC50";
    if (label.find(L"Copier le chemin")   != std::wstring::npos) return L"\uE71B";
    if (label.find(L"Ajouter")            != std::wstring::npos) return L"\uE710";
    if (label.find(L"Nouveau fichier")    != std::wstring::npos) return L"\uE7C3";
    if (label.find(L"Nouveau dossier")    != std::wstring::npos) return L"\uE8F4";
    if (label.find(L"Class Header")       != std::wstring::npos) return L"\uE943";
    if (label.find(L"Class Source")       != std::wstring::npos) return L"\uE943";
    if (label.find(L"Duplicate")          != std::wstring::npos) return L"\uE8C8";
    if (label.find(L"Rename")             != std::wstring::npos) return L"\uE8D6";
    if (label.find(L"Delete")             != std::wstring::npos) return L"\uE74D";
    if (label.find(L"Couper")             != std::wstring::npos) return L"\uE8C6";
    if (label.find(L"Copier")             != std::wstring::npos) return L"\uE8C8";
    if (label.find(L"Coller")             != std::wstring::npos) return L"\uE77F";
    if (label.find(L"Aller")              != std::wstring::npos) return L"\uE8A7";
    if (label.find(L"ggwave")             != std::wstring::npos) return L"\uE720";
    if (label.find(L"Deplacer")           != std::wstring::npos) return L"\uE8DE";
    return L"";
}

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

    // D2D resources
    ID2D1HwndRenderTarget  *rt         = nullptr;
    ID2D1SolidColorBrush   *brBg       = nullptr;
    ID2D1SolidColorBrush   *brBorder   = nullptr;
    ID2D1SolidColorBrush   *brHover    = nullptr;
    ID2D1SolidColorBrush   *brText     = nullptr;
    ID2D1SolidColorBrush   *brDisabled = nullptr;
    ID2D1SolidColorBrush   *brSep      = nullptr;
    ID2D1SolidColorBrush   *brIcon     = nullptr;
    ID2D1SolidColorBrush   *brDanger   = nullptr;
    IDWriteTextFormat      *tfLabel    = nullptr;
    IDWriteTextFormat      *tfShortcut = nullptr;
    IDWriteTextFormat      *tfIcon     = nullptr;

    bool CreateD2D(HWND hwnd)
    {
        if (!g_d2d || !g_dw) return false;

        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE));
        D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
            hwnd, D2D1::SizeU((UINT)winW, (UINT)winH), D2D1_PRESENT_OPTIONS_NONE);
        HRESULT hr = g_d2d->CreateHwndRenderTarget(rtProps, hwndProps, &rt);
        if (FAILED(hr) || !rt) return false;

        const UI::Theme::Palette &p = UI::Theme::GetPalette();

        rt->CreateSolidColorBrush(p.inputBackground, &brBg);
        rt->CreateSolidColorBrush(p.inputBorder,     &brBorder);

        D2D1_COLOR_F hov = p.explorerToolbarHover;
        hov.a = (UI::Theme::GetMode() == UI::Theme::Mode::Light) ? 0.90f : 1.0f;
        rt->CreateSolidColorBrush(hov, &brHover);

        rt->CreateSolidColorBrush(UI::Theme::PrimaryText(),  &brText);
        rt->CreateSolidColorBrush(UI::Theme::MutedText(),    &brDisabled);
        rt->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &brSep);

        D2D1_COLOR_F ic = UI::Theme::PrimaryText();
        ic.a *= 0.55f;
        rt->CreateSolidColorBrush(ic, &brIcon);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.35f, 0.35f, 0.85f), &brDanger);

        // Label format
        g_dw->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", &tfLabel);
        if (!tfLabel)
            g_dw->CreateTextFormat(L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", &tfLabel);
        if (tfLabel) {
            tfLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            tfLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Shortcut format (slightly smaller, right-aligned)
        g_dw->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &tfShortcut);
        if (!tfShortcut)
            g_dw->CreateTextFormat(L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &tfShortcut);
        if (tfShortcut) {
            tfShortcut->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            tfShortcut->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Icon format (Segoe MDL2 Assets)
        g_dw->CreateTextFormat(L"Segoe MDL2 Assets", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &tfIcon);
        if (tfIcon) {
            tfIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tfIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        return true;
    }

    void ReleaseD2D()
    {
        auto r = [](auto *&p) { if (p) { p->Release(); p = nullptr; } };
        r(tfLabel); r(tfShortcut); r(tfIcon);
        r(brBg); r(brBorder); r(brHover); r(brText);
        r(brDisabled); r(brSep); r(brIcon); r(brDanger);
        r(rt);
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

    // Background
    if (s->brBg)
        s->rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0.f, 0.f, W, H), 6.f, 6.f), s->brBg);

    // Border
    if (s->brBorder)
        s->rt->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), 5.5f, 5.5f),
            s->brBorder, 1.f);

    // Layout constants
    const float panelPad      =  5.f;
    const float iconPad       = 12.f;
    const float iconColW      = 22.f;
    const float textGap       =  8.f;
    const float rightPad      = 14.f;
    const float shortcutColW  = 80.f;

    bool hasShortcuts = false;
    for (const auto &sc : s->shortcuts) if (!sc.empty()) { hasShortcuts = true; break; }

    float y = kInnerPad;

    for (int i = 0; i < (int)s->items.size(); ++i)
    {
        // Separator line before this item
        bool hasSep = !s->separators.empty() && i < (int)s->separators.size() && s->separators[i];
        if (hasSep && s->brSep)
        {
            float sy = std::floor(y + kSepH * 0.5f) + 0.5f;
            s->rt->DrawLine(D2D1::Point2F(panelPad + 8.f, sy),
                            D2D1::Point2F(W - panelPad - 8.f, sy),
                            s->brSep, 1.f);
            y += kSepH;
        }

        D2D1_RECT_F itemRect = D2D1::RectF(panelPad, y, W - panelPad, y + kItemH);
        bool isEnabled = s->enabled.empty() || i >= (int)s->enabled.size() || s->enabled[i];

        // Hover highlight
        if (isEnabled && i == s->hover && s->brHover)
            s->rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(itemRect.left  + 2.f, itemRect.top    + 2.f,
                                               itemRect.right - 2.f, itemRect.bottom - 2.f),
                                  5.f, 5.f),
                s->brHover);

        ID2D1SolidColorBrush *textBrush = isEnabled ? s->brText : s->brDisabled;
        if (!s->tfLabel || !textBrush) { y += kItemH; continue; }

        const std::wstring &label = s->items[i];

        // Icon glyph
        std::wstring glyph = ContextIconGlyph(label);
        if (!glyph.empty() && s->tfIcon)
        {
            bool isDanger = (label.find(L"Delete")     != std::wstring::npos ||
                             label.find(L"Supprimer")  != std::wstring::npos);
            ID2D1SolidColorBrush *iconBrush = isEnabled
                ? (isDanger ? s->brDanger : s->brIcon)
                : s->brDisabled;
            D2D1_RECT_F iconRect = D2D1::RectF(
                itemRect.left + iconPad,
                itemRect.top,
                itemRect.left + iconPad + iconColW,
                itemRect.bottom);
            s->rt->DrawTextW(glyph.c_str(), (UINT32)glyph.size(),
                             s->tfIcon, iconRect, iconBrush,
                             D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        // Label
        float textLeft  = itemRect.left + iconPad + iconColW + textGap;
        float textRight = hasShortcuts ? (W - rightPad - shortcutColW) : (W - rightPad);
        D2D1_RECT_F textRect = D2D1::RectF(textLeft, itemRect.top, textRight, itemRect.bottom);
        s->rt->DrawTextW(label.c_str(), (UINT32)label.size(),
                         s->tfLabel, textRect, textBrush,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Shortcut
        if (hasShortcuts && s->tfShortcut && s->brDisabled &&
            !s->shortcuts.empty() && i < (int)s->shortcuts.size() &&
            !s->shortcuts[i].empty())
        {
            const std::wstring &sc = s->shortcuts[i];
            D2D1_RECT_F scRect = D2D1::RectF(textRight, itemRect.top, W - rightPad, itemRect.bottom);
            s->rt->DrawTextW(sc.c_str(), (UINT32)sc.size(),
                             s->tfShortcut, scRect, s->brDisabled,
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        y += kItemH;
    }

    HRESULT hr = s->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        // Render target lost — recreate on next paint
        s->ReleaseD2D();
    }
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

    bool hasShortcuts = !shortcuts.empty() &&
        std::any_of(shortcuts.begin(), shortcuts.end(),
                    [](const std::wstring &s){ return !s.empty(); });

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
    s->winW    = hasShortcuts ? 310 : 268;
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
        wc.style         = CS_DROPSHADOW;   // OS-level drop shadow — free!
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
