#include "CustomPopup.h"
#include "utils/logger/Logger.h"
#include <windowsx.h>
#include <sstream>

static const wchar_t* POPUP_CLASS = L"NebulaCustomPopup";

struct PopupState {
    std::vector<std::wstring> items;
    int hoverIndex = -1;
    HWND parent;
    int baseId;
    bool mouseInside = true;
};

static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PopupState* s = reinterpret_cast<PopupState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    
    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = reinterpret_cast<PopupState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        
        // Track mouse pour détecter quand la souris sort
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        
        Logger::Instance().Log(L"CustomPopup: WM_CREATE");
        return 0;
    }
    
    case WM_MOUSEMOVE: {
        if (!s) break;
        
        // Re-track mouse si nécessaire
        if (!s->mouseInside) {
            s->mouseInside = true;
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc; 
        GetClientRect(hwnd, &rc);
        
        int itemH = (rc.bottom - rc.top) / max(1, (int)s->items.size());
        int idx = pt.y / max(1, itemH);
        
        if (idx < 0 || idx >= (int)s->items.size()) {
            idx = -1;
        }
        
        if (idx != s->hoverIndex) {
            s->hoverIndex = idx;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    
    case WM_MOUSELEAVE: {
        if (!s) break;
        s->mouseInside = false;
        // Ne fermer que si la souris sort vraiment de la zone
        POINT pt;
        GetCursorPos(&pt);
        RECT rc;
        GetWindowRect(hwnd, &rc);
        Logger::Instance().Log(L"CustomPopup: WM_MOUSELEAVE");
        if (!PtInRect(&rc, pt)) {
            Logger::Instance().Log(L"CustomPopup: WM_MOUSELEAVE -> DestroyWindow");
            DestroyWindow(hwnd);
        }
        return 0;
    }
    
    case WM_LBUTTONDOWN: {
        if (!s) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc; 
        GetClientRect(hwnd, &rc);
        
        int itemH = (rc.bottom - rc.top) / max(1, (int)s->items.size());
        int idx = pt.y / max(1, itemH);
        
        if (idx >= 0 && idx < (int)s->items.size()) {
            s->hoverIndex = idx;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    
    case WM_LBUTTONUP: {
        if (!s) break;
        
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc; 
        GetClientRect(hwnd, &rc);
        
        int itemH = (rc.bottom - rc.top) / max(1, (int)s->items.size());
        int idx = pt.y / max(1, itemH);
        
        if (idx >= 0 && idx < (int)s->items.size()) {
            // Envoyer le message au parent
            Logger::Instance().Log(std::wstring(L"CustomPopup: item clicked idx=") + std::to_wstring(idx));
            PostMessageW(s->parent, WM_COMMAND, (WPARAM)(s->baseId + idx), 0);
        }

        // Fermer après un court délai pour laisser le message être traité
        Logger::Instance().Log(L"CustomPopup: DestroyWindow (on LBUTTONUP)");
        DestroyWindow(hwnd);
        return 0;
    }
    
    case WM_PAINT: {
        if (!s) break;
        PAINTSTRUCT ps; 
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; 
        GetClientRect(hwnd, &rc);
        
        // Background avec bordure
        HBRUSH bg = CreateSolidBrush(RGB(40, 40, 40));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        
        // Bordure
        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
        HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(borderPen);
        
        // Draw items
        int count = (int)s->items.size();
        int itemH = max(1, (rc.bottom - rc.top) / max(1, count));
        
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT old = (HFONT)SelectObject(hdc, f);
        SetBkMode(hdc, TRANSPARENT);
        
        for (int i = 0; i < count; i++) {
            RECT ir = { 
                rc.left + 2, 
                rc.top + i * itemH + 2, 
                rc.right - 2, 
                rc.top + (i + 1) * itemH - 2 
            };
            
            // Hover background
            if (i == s->hoverIndex) {
                HBRUSH hb = CreateSolidBrush(RGB(60, 60, 60));
                FillRect(hdc, &ir, hb);
                DeleteObject(hb);
            }
            
            // Texte
            RECT textRect = { ir.left + 8, ir.top, ir.right - 8, ir.bottom };
            SetTextColor(hdc, RGB(220, 220, 220));
            DrawTextW(hdc, s->items[i].c_str(), (int)s->items[i].size(), 
                     &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        }
        
        SelectObject(hdc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    
    case WM_DESTROY: {
        Logger::Instance().Log(L"CustomPopup: WM_DESTROY");
        if (s) {
            delete s;
        }
        return 0;
    }
    }
    
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ShowCustomPopup(HWND parent, const std::vector<std::wstring>& items, POINT screenPos, int baseId) {
    // Register class if needed
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(GetModuleHandleW(NULL), POPUP_CLASS, &wc)) {
        wc.lpfnWndProc = PopupWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = POPUP_CLASS;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
    }

    // Calculer la taille
    HDC screen = GetDC(NULL);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT oldf = (HFONT)SelectObject(screen, f);
    
    int maxw = 100; // Largeur minimum
    SIZE sz;
    for (auto &it : items) {
        GetTextExtentPoint32W(screen, it.c_str(), (int)it.size(), &sz);
        if (sz.cx > maxw) maxw = sz.cx;
    }
    
    SelectObject(screen, oldf);
    ReleaseDC(NULL, screen);

    int padding = 32; // Plus de padding pour être lisible
    int itemH = 28;   // Hauteur plus confortable
    int w = maxw + padding;
    int h = itemH * (int)items.size() + 4; // +4 pour la bordure

    // Créer l'état
    PopupState* s = new PopupState();
    s->items = items;
    s->parent = parent;
    s->baseId = baseId;

    // Créer la fenêtre popup
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        POPUP_CLASS, 
        L"", 
        WS_POPUP,
        screenPos.x, 
        screenPos.y, 
        w, 
        h, 
        parent, 
        NULL, 
        GetModuleHandleW(NULL), 
        s
    );
    
    if (!hwnd) {
        delete s;
        return;
    }

    // Log popup creation details
    {
        std::wostringstream ss;
        ss << L"CustomPopup: CreateWindowEx hwnd=" << (void*)hwnd << L" pos=(" << screenPos.x << L"," << screenPos.y << L") size=(" << w << L"," << h << L") baseId=" << s->baseId;
        Logger::Instance().Log(ss.str());
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
}