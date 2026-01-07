#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>

// Structure pour un item de menu
struct MenuItem
{
    std::wstring label;
    D2D1_RECT_F rect;
    bool hovered;
};

// Structure pour le dropdown menu
struct MenuDropdown
{
    int menuIndex;
    std::vector<std::wstring> items;
    D2D1_RECT_F rect;
    int hoveredItem;
    bool visible;
    int baseId = 0;
    std::vector<bool> enabled; // per-item enabled/disabled state
};

// Fonctions principales
void DrawCustomTitleBarD2D(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd, int hoveredButton, bool hasFocus, const std::wstring &title);

// Fonctions pour les menus
int GetHoveredMenuItem(HWND hwnd, POINT pt);
void SetMenuItemHovered(int index, bool hovered);
std::vector<MenuItem> &GetMenuItems();

// Fonctions pour le dropdown
void ShowMenuDropdown(HWND hwnd, int menuIndex, D2D1_RECT_F menuRect);
void HideMenuDropdown(HWND hwnd);
bool IsMenuDropdownVisible();
void DrawMenuDropdown(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
int GetDropdownHoveredItem(POINT pt);
void SetDropdownHoveredItem(int index);
bool IsPointInDropdown(POINT pt);
MenuDropdown &GetActiveDropdown();
void ShowContextMenuDropdown(HWND hwnd, const std::vector<std::wstring>& items, D2D1_POINT_2F position, int baseId);
