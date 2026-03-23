#include "PopupWindow.h"
#include "utils/logger/Logger.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include "helpers/window_helpers.h"

static const wchar_t *POPUP_WINDOW_CLASS = L"NebulaPopupWindow";
static HWND g_activePopup = nullptr;
static bool g_popupDragging = false;
static POINT g_popupDragOffset = {0, 0};
static ID2D1Factory *g_d2dFactory = nullptr;
static IDWriteFactory *g_dwriteFactory = nullptr;

struct PopupWindowState
{
    std::wstring title;
    std::wstring message;
    bool hoverClose = false;
    RECT closeRect = {0, 0, 0, 0};
    bool focused = true;
};

template <typename T>
static void SafeRelease(T *&p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

static int GetHeaderHeightPx(HWND hwnd)
{
    UINT dpi = win32_get_dpi_for_window(hwnd);
    float scale = static_cast<float>(dpi) / 96.0f;
    int headerH = static_cast<int>(std::lround(36.0f * scale));
    return (std::max)(30, headerH);
}

static RECT GetCloseRect(HWND hwnd, const RECT &rc)
{
    int headerH = GetHeaderHeightPx(hwnd);
    RECT r = {rc.right - headerH, rc.top, rc.right, rc.top + headerH};
    return r;
}

static void DrawCloseButton(ID2D1HwndRenderTarget *rt, const RECT &r, bool hovered)
{
    if (!rt)
        return;

    ID2D1SolidColorBrush *xBrush = nullptr;
    ID2D1SolidColorBrush *hoverBrush = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.88f, 0.88f, 0.88f, 1.0f), &xBrush);
    if (hovered)
        rt->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.18f, 0.18f, 1.0f), &hoverBrush);
    if (hoverBrush)
    {
        rt->FillRectangle(D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom), hoverBrush);
        hoverBrush->Release();
    }

    if (xBrush)
    {
        float centerX = (r.left + r.right) * 0.5f;
        float centerY = (r.top + r.bottom) * 0.5f;
        float size = (std::max)(8.0f, (r.bottom - r.top) * 0.30f);
        float half = size * 0.5f;
        rt->DrawLine(D2D1::Point2F(centerX - half, centerY - half),
                     D2D1::Point2F(centerX + half, centerY + half), xBrush, 1.25f);
        rt->DrawLine(D2D1::Point2F(centerX - half, centerY + half),
                     D2D1::Point2F(centerX + half, centerY - half), xBrush, 1.25f);
        xBrush->Release();
    }
}

static void ApplyPopupDwmStyle(HWND hwnd, bool focused)
{
    (void)focused;

    // Remove native DWM border entirely (avoids bright/white border artifacts).
    const DWORD DWMWA_BORDER_COLOR = 34;
    COLORREF borderNone = (COLORREF)0xFFFFFFFEu;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderNone, sizeof(borderNone));

    const DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    const int DWMWCP_ROUND = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &DWMWCP_ROUND, sizeof(DWMWCP_ROUND));
}

static LRESULT CALLBACK PopupWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PopupWindowState *s = reinterpret_cast<PopupWindowState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_NCCALCSIZE:
        if (wParam)
            return 0;
        {
            UINT dpi = win32_get_dpi_for_window(hwnd);
            int frame_x = win32_get_system_metrics_for_dpi(SM_CXFRAME, dpi);
            int frame_y = win32_get_system_metrics_for_dpi(SM_CYFRAME, dpi);
            int padding = win32_get_system_metrics_for_dpi(SM_CXPADDEDBORDER, dpi);
            RECT *rc = (RECT *)lParam;
            rc->left += frame_x + padding;
            rc->right -= frame_x + padding;
            rc->bottom -= frame_y + padding;
            return 0;
        }
    case WM_CREATE:
    {
        CREATESTRUCTW *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        s = reinterpret_cast<PopupWindowState *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        ApplyPopupDwmStyle(hwnd, true);

        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_ACTIVATE:
    {
        if (!s)
            break;
        s->focused = (LOWORD(wParam) != WA_INACTIVE);
        ApplyPopupDwmStyle(hwnd, s->focused);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
    {
        if (g_activePopup == hwnd)
            g_activePopup = nullptr;
        if (s)
            delete s;
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT:
    {
        if (!s)
            break;
        if (!g_d2dFactory)
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory);
        if (!g_dwriteFactory)
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g_dwriteFactory);

        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        UINT dpi = win32_get_dpi_for_window(hwnd);
        float scale = (float)dpi / 96.0f;
        int headerH = GetHeaderHeightPx(hwnd);

        ID2D1HwndRenderTarget *rt = nullptr;
        if (g_d2dFactory)
        {
            D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
                0, 0, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
            D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
                hwnd, D2D1::SizeU((UINT)(rc.right - rc.left), (UINT)(rc.bottom - rc.top)));
            g_d2dFactory->CreateHwndRenderTarget(rtProps, hwndProps, &rt);
        }

        if (rt && g_dwriteFactory)
        {
            ID2D1SolidColorBrush *bgBrush = nullptr;
            ID2D1SolidColorBrush *headerBrush = nullptr;
            ID2D1SolidColorBrush *titleBrush = nullptr;
            ID2D1SolidColorBrush *bodyBrush = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.09f, 0.10f, 0.11f, 0.98f), &bgBrush);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.13f, 1.0f), &headerBrush);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.94f, 0.96f, 1.0f), &titleBrush);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.82f, 0.84f, 0.87f, 1.0f), &bodyBrush);

            IDWriteTextFormat *titleFmt = nullptr;
            IDWriteTextFormat *bodyFmt = nullptr;
            g_dwriteFactory->CreateTextFormat(
                L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f * scale, L"fr-fr", &titleFmt);
            g_dwriteFactory->CreateTextFormat(
                L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * scale, L"fr-fr", &bodyFmt);
            if (titleFmt)
            {
                titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            if (bodyFmt)
            {
                bodyFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                bodyFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                bodyFmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 20.0f * scale, 16.0f * scale);
            }

            rt->BeginDraw();
            rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            D2D1_RECT_F bounds = D2D1::RectF((float)rc.left, (float)rc.top, (float)rc.right, (float)rc.bottom);
            D2D1_RECT_F header = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + (float)headerH);
            if (bgBrush)
                rt->FillRectangle(bounds, bgBrush);
            if (headerBrush)
                rt->FillRectangle(header, headerBrush);

            float padX = 12.0f * scale;
            float padY = 10.0f * scale;
            if (!s->title.empty() && titleFmt && titleBrush)
            {
                D2D1_RECT_F tr = D2D1::RectF(
                    std::round(bounds.left + padX),
                    std::round(bounds.top),
                    std::round(bounds.right - (headerH + 8.0f * scale)),
                    std::round(bounds.top + (float)headerH));
                rt->DrawTextW(s->title.c_str(), (UINT32)s->title.size(), titleFmt, tr, titleBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
            if (!s->message.empty() && bodyFmt && bodyBrush)
            {
                D2D1_RECT_F mr = D2D1::RectF(
                    std::round(bounds.left + padX),
                    std::round(bounds.top + (float)headerH + padY),
                    std::round(bounds.right - padX),
                    std::round(bounds.bottom - padY));
                rt->DrawTextW(s->message.c_str(), (UINT32)s->message.size(), bodyFmt, mr, bodyBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }

            s->closeRect = GetCloseRect(hwnd, rc);
            DrawCloseButton(rt, s->closeRect, s->hoverClose);
            rt->EndDraw();

            SafeRelease(titleFmt);
            SafeRelease(bodyFmt);
            SafeRelease(bgBrush);
            SafeRelease(headerBrush);
            SafeRelease(titleBrush);
            SafeRelease(bodyBrush);
        }

        SafeRelease(rt);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND ShowPopupWindow(HWND owner, int x, int y, int width, int height,
                     const std::wstring &title, const std::wstring &message)
{
    (void)owner;
    if (g_activePopup)
    {
        DestroyWindow(g_activePopup);
        g_activePopup = nullptr;
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(GetModuleHandleW(NULL), POPUP_WINDOW_CLASS, &wc))
    {
        wc.lpfnWndProc = PopupWindowProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = POPUP_WINDOW_CLASS;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.style = CS_DBLCLKS;
        RegisterClassExW(&wc);
    }

    PopupWindowState *s = new PopupWindowState();
    s->title = title;
    s->message = message;

    DWORD style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT;

    HWND hwnd = CreateWindowExW(
        exStyle,
        POPUP_WINDOW_CLASS,
        L"",
        style,
        x,
        y,
        width,
        height,
        NULL,
        NULL,
        GetModuleHandleW(NULL),
        s);

    if (!hwnd)
    {
        delete s;
        return NULL;
    }

    g_activePopup = hwnd;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    std::wostringstream ss;
    ss << L"PopupWindow: CreateWindowEx hwnd=" << (void *)hwnd << L" pos=(" << x << L"," << y << L") size=(" << width << L"," << height << L")";
    Logger::Instance().Log(ss.str());

    return hwnd;
}

static bool GetPopupRect(RECT &out)
{
    if (!g_activePopup)
        return false;
    return GetWindowRect(g_activePopup, &out) != 0;
}

bool PopupHitTestClose(POINT screenPt)
{
    RECT rc;
    if (!GetPopupRect(rc))
        return false;
    RECT closeR = GetCloseRect(g_activePopup, rc);
    return PtInRect(&closeR, screenPt);
}

bool PopupHitTestHeader(POINT screenPt)
{
    RECT rc;
    if (!GetPopupRect(rc))
        return false;
    RECT header = rc;
    header.bottom = header.top + GetHeaderHeightPx(g_activePopup);
    return PtInRect(&header, screenPt);
}

void PopupSetHoverClose(bool hovered)
{
    if (!g_activePopup)
        return;
    PopupWindowState *s = reinterpret_cast<PopupWindowState *>(GetWindowLongPtrW(g_activePopup, GWLP_USERDATA));
    if (!s)
        return;
    if (s->hoverClose == hovered)
        return;
    s->hoverClose = hovered;
    RECT rc;
    GetClientRect(g_activePopup, &rc);
    s->closeRect = GetCloseRect(g_activePopup, rc);
    InvalidateRect(g_activePopup, &s->closeRect, FALSE);
}

void PopupStartDrag(POINT screenPt)
{
    RECT rc;
    if (!GetPopupRect(rc))
        return;
    g_popupDragging = true;
    g_popupDragOffset.x = screenPt.x - rc.left;
    g_popupDragOffset.y = screenPt.y - rc.top;
}

void PopupDragTo(POINT screenPt)
{
    if (!g_popupDragging || !g_activePopup)
        return;
    RECT rc;
    if (!GetPopupRect(rc))
        return;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    int x = screenPt.x - g_popupDragOffset.x;
    int y = screenPt.y - g_popupDragOffset.y;
    SetWindowPos(g_activePopup, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void PopupEndDrag()
{
    g_popupDragging = false;
}

bool PopupIsDragging()
{
    return g_popupDragging;
}

HWND GetActivePopupWindow()
{
    return g_activePopup;
}

bool CloseActivePopupWindow()
{
    if (!g_activePopup)
        return false;
    DestroyWindow(g_activePopup);
    return true;
}
