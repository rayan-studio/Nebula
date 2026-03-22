#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

class Window;

namespace UI
{
struct NewProjectRenderContext
{
    float scale = 1.0f;
    D2D1_RECT_F full = {};
    D2D1_RECT_F leftPanel = {};
    D2D1_RECT_F rightPanel = {};
    const wchar_t *uiFont = nullptr;
    const wchar_t *iconFont = nullptr;
    IDWriteFontCollection *uiCollection = nullptr;

    ID2D1SolidColorBrush *panelBg = nullptr;
    ID2D1SolidColorBrush *panelBorder = nullptr;
    ID2D1SolidColorBrush *divider = nullptr;
    ID2D1SolidColorBrush *muted = nullptr;
    ID2D1SolidColorBrush *text = nullptr;
    ID2D1SolidColorBrush *subtle = nullptr;
    ID2D1SolidColorBrush *accent = nullptr;

    IDWriteTextFormat *titleFmt = nullptr;
    IDWriteTextFormat *sectionFmt = nullptr;
    IDWriteTextFormat *labelFmt = nullptr;
    IDWriteTextFormat *itemFmt = nullptr;
    IDWriteTextFormat *subFmt = nullptr;
    IDWriteTextFormat *iconFmt = nullptr;
    IDWriteTextFormat *actionFmt = nullptr;
};

class NewProjectHomeView
{
public:
    void Draw(Window &window, ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, const NewProjectRenderContext &rc);
};

class NewProjectCreateView
{
public:
    void Draw(Window &window, ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, const NewProjectRenderContext &rc);
};

class NewProjectOverlay
{
public:
    void Draw(Window &window, ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, const RECT &clientRect);
    bool HandleMouseDown(Window &window, HWND hwnd, POINT pt);
    bool HandleMouseUp(Window &window, HWND hwnd, POINT pt);
    bool HandleMouseMove(Window &window, HWND hwnd, POINT pt);
    bool HandleChar(Window &window, wchar_t ch);
    bool HandleKeyDown(Window &window, WPARAM key);
};
} // namespace UI
