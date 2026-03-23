#include "helpers/window_helpers.h"
#include <uxtheme.h>
#include <vssym32.h>
#include <cmath>

#pragma comment(lib, "uxtheme.lib")

int win32_dpi_scale(int value, UINT dpi) {
    return static_cast<int>(std::lround(static_cast<float>(value) * static_cast<float>(dpi) / 96.0f));
}

UINT win32_get_dpi_for_window(HWND handle) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiForWindow_t = UINT(WINAPI *)(HWND);
        auto pGetDpiForWindow = reinterpret_cast<GetDpiForWindow_t>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (pGetDpiForWindow)
            return pGetDpiForWindow(handle);
    }
    HDC hdc = GetDC(handle);
    UINT dpi = 96;
    if (hdc) {
        dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(handle, hdc);
    }
    return dpi ? dpi : 96;
}

int win32_get_system_metrics_for_dpi(int metric, UINT dpi) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetSystemMetricsForDpi_t = int(WINAPI *)(int, UINT);
        auto pGetSystemMetricsForDpi = reinterpret_cast<GetSystemMetricsForDpi_t>(
            GetProcAddress(user32, "GetSystemMetricsForDpi"));
        if (pGetSystemMetricsForDpi)
            return pGetSystemMetricsForDpi(metric, dpi);
    }
    return GetSystemMetrics(metric);
}

bool win32_window_is_maximized(HWND handle) {
    WINDOWPLACEMENT placement = {0};
    placement.length = sizeof(WINDOWPLACEMENT);
    if (GetWindowPlacement(handle, &placement)) {
        return placement.showCmd == SW_SHOWMAXIMIZED;
    }
    return false;
}

RECT win32_titlebar_rect(HWND handle) {
    // Use a fixed title bar height of 35 logical pixels (DPI-scaled)
    const int fixed_height = 35; // logical pixels
    UINT dpi = win32_get_dpi_for_window(handle);
    int height = win32_dpi_scale(fixed_height, dpi);

    RECT rect;
    GetClientRect(handle, &rect);
    rect.bottom = rect.top + height;

    if (win32_window_is_maximized(handle)) {
        int frame_y = win32_get_system_metrics_for_dpi(SM_CYFRAME, dpi);
        rect.top -= frame_y;
        rect.bottom -= frame_y;
    }

    return rect;
}

RECT win32_fake_shadow_rect(HWND handle) {
    RECT rect;
    GetClientRect(handle, &rect);
    rect.bottom = rect.top + WIN32_FAKE_SHADOW_HEIGHT;
    return rect;
}

CustomTitleBarButtonRects win32_get_title_bar_button_rects(HWND handle, const RECT *title_bar_rect) {
    UINT dpi = win32_get_dpi_for_window(handle);
    CustomTitleBarButtonRects button_rects;
    int button_width = win32_dpi_scale(47, dpi);
    button_rects.close = *title_bar_rect;
    button_rects.close.top += WIN32_FAKE_SHADOW_HEIGHT;

    button_rects.close.left = button_rects.close.right - button_width;
    button_rects.maximize = button_rects.close;
    button_rects.maximize.left -= button_width;
    button_rects.maximize.right -= button_width;
    button_rects.minimize = button_rects.maximize;
    button_rects.minimize.left -= button_width;
    button_rects.minimize.right -= button_width;
    button_rects.run = button_rects.minimize;
    button_rects.run.left -= button_width;
    button_rects.run.right -= button_width;
    return button_rects;
}

void win32_center_rect_in_rect(RECT *to_center, const RECT *outer_rect) {
    int to_width = to_center->right - to_center->left;
    int to_height = to_center->bottom - to_center->top;
    int outer_width = outer_rect->right - outer_rect->left;
    int outer_height = outer_rect->bottom - outer_rect->top;

    int padding_x = (outer_width - to_width) / 2;
    int padding_y = (outer_height - to_height) / 2;

    to_center->left = outer_rect->left + padding_x;
    to_center->top = outer_rect->top + padding_y;
    to_center->right = to_center->left + to_width;
    to_center->bottom = to_center->top + to_height;
}

void set_menu_item_state(HMENU menu, MENUITEMINFO* menuItemInfo, UINT item, bool enabled) {
    menuItemInfo->fState = enabled ? MF_ENABLED : MF_DISABLED;
    SetMenuItemInfo(menu, item, false, menuItemInfo);
}
