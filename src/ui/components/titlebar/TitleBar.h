#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>

// Fonctions principales
void DrawCustomTitleBarD2D(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd, int hoveredButton, bool hasFocus, const std::wstring &title);
