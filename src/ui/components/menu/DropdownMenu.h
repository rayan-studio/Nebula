#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>

// Layout constants shared between DropdownMenu.cpp and its callers
inline constexpr float kDropdownItemH    = 30.0f;
inline constexpr float kDropdownSepH     = 6.0f;
inline constexpr float kDropdownInnerPad = 4.0f;

struct MenuItem
{
    std::wstring label;
    D2D1_RECT_F rect;
    bool hovered;
};

struct MenuDropdown
{
    int menuIndex;
    std::vector<std::wstring> items;
    std::vector<std::wstring> shortcuts;
    std::vector<wchar_t> icons;
    std::vector<bool> separators;
    std::vector<bool> hasSubmenu;
    D2D1_RECT_F rect;
    int hoveredItem;
    bool visible;
    int baseId = 0;
    std::vector<bool> enabled;
};

int GetHoveredMenuItem(HWND hwnd, POINT pt);
void SetMenuItemHovered(int index, bool hovered);
std::vector<MenuItem> &GetMenuItems();

void ShowMenuDropdown(HWND hwnd, int menuIndex, D2D1_RECT_F menuRect);
void HideMenuDropdown(HWND hwnd);
bool IsMenuDropdownVisible();
void DrawMenuDropdown(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
void DrawDropdownPanel(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, MenuDropdown &dd);
int GetDropdownHoveredItem(POINT pt);
void SetDropdownHoveredItem(int index);
bool IsPointInDropdown(POINT pt);
MenuDropdown &GetActiveDropdown();
// separators[i] = true  →  draw a thin separator line BEFORE item i
// enabled[i]    = false →  item is greyed out and non-clickable
void ShowContextMenuDropdown(HWND hwnd, const std::vector<std::wstring> &items, D2D1_POINT_2F position, int baseId,
                             const std::vector<bool>         &separators = {},
                             const std::vector<std::wstring> &shortcuts  = {},
                             const std::vector<bool>         &enabled    = {});

void ShowSubmenuDropdown(HWND hwnd, const std::vector<std::wstring> &items, D2D1_POINT_2F position, int baseId,
                         const std::vector<bool> &separators = {});
void HideSubmenuDropdown(HWND hwnd);
bool IsSubmenuDropdownVisible();
void DrawSubmenuDropdown(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
int GetSubmenuHoveredItem(POINT pt);
void SetSubmenuHoveredItem(int index);
bool IsPointInSubmenu(POINT pt);
MenuDropdown &GetSubmenuDropdown();
