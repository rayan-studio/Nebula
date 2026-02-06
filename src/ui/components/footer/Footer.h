#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <string>

// Draw a footer bar at the bottom of the window. The footer uses the same
// background and top border color as the title bar. It displays Ln/Col and
// the language of the active editor file.
void DrawFooterD2D(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd, const std::wstring &filePath, int line, int column, const std::wstring &encoding);

// Mouse handling for footer hover effects
void Footer_OnMouseMove(HWND hwnd, POINT pt);
void Footer_ClearHover(HWND hwnd);

// Footer hint text (short-lived UI hints like shortcut prompts)
void Footer_SetHint(HWND hwnd, const std::wstring &text, unsigned int durationMs = 2000);
void Footer_ClearHint(HWND hwnd);
