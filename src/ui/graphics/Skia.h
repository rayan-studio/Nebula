#pragma once
// Prevent Windows headers from defining min/max macros which break std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <d2d1.h>
#include <dwrite.h>
#include <string>

class SKText;

class Skia {
public:
    Skia();
    ~Skia();

    bool Init(HWND hwnd);
    void Render(const std::wstring& text, HWND hwnd, int titlebarHoveredButton, bool titlebarHasFocus, const std::wstring& titleText, HDC hdc = nullptr);
    void Resize(UINT width, UINT height);

    // Expose render target and DWrite factory for titlebar rendering if needed
    ID2D1RenderTarget* GetRenderTarget() { return pRenderTarget_; }
    IDWriteFactory* GetDWriteFactory() { return pDWriteFactory_; }

private:
    HWND hwnd_;
    ID2D1Factory *pFactory_;
    ID2D1DCRenderTarget *pRenderTarget_;
    IDWriteFactory *pDWriteFactory_;
    SKText *pSKText_;
    UINT surfaceWidth_, surfaceHeight_;
};

class SKText {
public:
    SKText(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    ~SKText();

    void Draw(const std::wstring& text);

private:
    ID2D1RenderTarget* pTarget_;
    IDWriteTextFormat* pTextFormat_;
    IDWriteFactory* pDWriteFactory_;
    ID2D1SolidColorBrush* pBrush_;
    bool initialized_;
    D2D1_RECT_F fixedLayoutRect_;
    float currentFontSize_;
    // recreate text format for a given font size
    void EnsureTextFormat(float fontSize);
};
