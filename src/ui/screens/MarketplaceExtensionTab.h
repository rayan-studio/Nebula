#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>

class MarketplaceExtensionTabView
{
public:
    void SetLibraryName(const std::wstring &name);
    void UpdateLayout(HWND hwnd, float left, float top, float right, float bottom);
    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);

    void OnMouseMove(HWND hwnd, POINT clientPoint);
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint);
    void OnLeftButtonUp(HWND hwnd);
    bool IsPointInView(POINT clientPoint) const;

private:
    std::wstring currentLibraryName_;
    D2D1_RECT_F bounds_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F cardRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F actionButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    bool actionButtonHovered_ = false;

    bool IsPointInRect(POINT pt, const D2D1_RECT_F &rect) const;
};
