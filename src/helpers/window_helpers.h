#pragma once
#include <Windows.h>

// Small helpers extracted from Window.cpp to keep code organized.

#ifndef WIN32_FAKE_SHADOW_HEIGHT
#define WIN32_FAKE_SHADOW_HEIGHT 1
#endif

#ifndef WIN32_MAXIMIZED_RECTANGLE_OFFSET
#define WIN32_MAXIMIZED_RECTANGLE_OFFSET 2
#endif

typedef struct {
    RECT close;
    RECT maximize;
    RECT minimize;
    RECT run;
    RECT debug;
} CustomTitleBarButtonRects;

int win32_dpi_scale(int value, UINT dpi);
UINT win32_get_dpi_for_window(HWND handle);
int win32_get_system_metrics_for_dpi(int metric, UINT dpi);
bool win32_window_is_maximized(HWND handle);
RECT win32_titlebar_rect(HWND handle);
RECT win32_fake_shadow_rect(HWND handle);
CustomTitleBarButtonRects win32_get_title_bar_button_rects(HWND handle, const RECT *title_bar_rect);
void win32_center_rect_in_rect(RECT *to_center, const RECT *outer_rect);
void set_menu_item_state(HMENU menu, MENUITEMINFO* menuItemInfo, UINT item, bool enabled);
