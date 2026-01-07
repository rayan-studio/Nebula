#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <string>

namespace UI {
    // Draw the welcome screen (centered icon + title + shortcuts) into the provided D2D render target.
    // editor bounds are in physical pixels (left, top, right, bottom)
    void DrawWelcomeD2D(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd, const std::wstring &customFontPath,
                        float editorLeft, float editorTop, float editorRight, float editorBottom);
    // CPU-only welcome renderer that draws directly to an HDC (no GPU/D3D initialization).
    // Call this after D2D EndDraw while you still have a valid HDC bound.
    void DrawWelcomeCPU(HDC hdc, HWND hwnd, const std::wstring &customFontPath,
                        float editorLeft, float editorTop, float editorRight, float editorBottom);
}
