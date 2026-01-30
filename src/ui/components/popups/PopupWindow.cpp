#include "PopupWindow.h"
#include "utils/logger/Logger.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <algorithm>
#include <sstream>

static const wchar_t *POPUP_WINDOW_CLASS = L"NebulaPopupWindow";

struct PopupWindowState
{
    std::wstring title;
    std::wstring message;
    bool hoverClose = false;
    RECT closeRect = {0, 0, 0, 0};
    bool focused = true;
};

static RECT GetCloseRect(const RECT &rc)
{
    const int headerH = 34;
    RECT r = {rc.right - headerH, rc.top, rc.right, rc.top + headerH};
    return r;
}

static bool PointInRectClient(POINT pt, const RECT &r)
{
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

static void DrawCloseButton(HDC hdc, const RECT &r, bool hovered)
{
    if (hovered)
    {
        HBRUSH hb = CreateSolidBrush(RGB(45, 45, 45));
        FillRect(hdc, &r, hb);
        DeleteObject(hb);
    }

    int centerX = (r.left + r.right) / 2;
    int centerY = (r.top + r.bottom) / 2;
    int size = 9;
    int half = size / 2;
    int left = centerX - half;
    int right = centerX + half;
    int top = centerY - half;
    int bottom = centerY + half;

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(220, 220, 220));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, left, top, nullptr);
    LineTo(hdc, right, bottom);
    MoveToEx(hdc, left, bottom, nullptr);
    LineTo(hdc, right, top);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static void ApplyPopupDwmStyle(HWND hwnd, bool focused)
{
    const DWORD DWMWA_BORDER_COLOR = 34;
    COLORREF color = focused ? RGB(61, 143, 242) : RGB(51, 51, 51);
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));

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
        return 0;
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
    case WM_MOUSELEAVE:
    {
        if (!s)
            break;
        if (s->hoverClose)
        {
            s->hoverClose = false;
            InvalidateRect(hwnd, &s->closeRect, FALSE);
        }

        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!s)
            break;
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT closeR = GetCloseRect(rc);
        bool hovered = PointInRectClient(pt, closeR);
        if (hovered != s->hoverClose)
        {
            s->hoverClose = hovered;
            s->closeRect = closeR;
            InvalidateRect(hwnd, &closeR, FALSE);
        }
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        if (!s)
            break;
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT closeR = GetCloseRect(rc);
        const int headerH = 34;
        if (PointInRectClient(pt, closeR))
            return 0;
        if (pt.y >= rc.top && pt.y < rc.top + headerH)
        {
            POINT screenPt = pt;
            ClientToScreen(hwnd, &screenPt);
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(screenPt.x, screenPt.y));
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (!s)
            break;
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT closeR = GetCloseRect(rc);
        if (PointInRectClient(pt, closeR))
        {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_PAINT:
    {
        if (!s)
            break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Background
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 24));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        // Border handled by DWM (thin), avoid double-stroking here.

        // Header line
        RECT header = rc;
        header.bottom = header.top + 34;
        HBRUSH hb = CreateSolidBrush(RGB(28, 28, 28));
        FillRect(hdc, &header, hb);
        DeleteObject(hb);

        // Title text
        if (!s->title.empty())
        {
            HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT old = (HFONT)SelectObject(hdc, f);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(220, 220, 220));
            RECT tr = header;
            tr.left += 10;
            tr.right -= 44;
            DrawTextW(hdc, s->title.c_str(), (int)s->title.size(), &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            SelectObject(hdc, old);
        }

        if (!s->message.empty())
        {
            HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT old = (HFONT)SelectObject(hdc, f);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(190, 190, 190));
            RECT mr = rc;
            mr.left += 12;
            mr.right -= 12;
            mr.top = header.bottom + 10;
            DrawTextW(hdc, s->message.c_str(), (int)s->message.size(), &mr,
                      DT_WORDBREAK | DT_LEFT | DT_TOP);
            SelectObject(hdc, old);
        }

        // Close button
        s->closeRect = GetCloseRect(rc);
        DrawCloseButton(hdc, s->closeRect, s->hoverClose);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND ShowPopupWindow(HWND owner, int x, int y, int width, int height,
                     const std::wstring &title, const std::wstring &message)
{
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

    DWORD style = WS_OVERLAPPED | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    DWORD exStyle = WS_EX_TOOLWINDOW;

    HWND hwnd = CreateWindowExW(
        exStyle,
        POPUP_WINDOW_CLASS,
        L"",
        style,
        x,
        y,
        width,
        height,
        owner,
        NULL,
        GetModuleHandleW(NULL),
        s);

    if (!hwnd)
    {
        delete s;
        return NULL;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    std::wostringstream ss;
    ss << L"PopupWindow: CreateWindowEx hwnd=" << (void *)hwnd << L" pos=(" << x << L"," << y << L") size=(" << width << L"," << height << L")";
    Logger::Instance().Log(ss.str());

    return hwnd;
}
