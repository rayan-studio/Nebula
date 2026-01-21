#pragma once
#include "ui/panels/Panel.h"

class SettingsPanel : public Panel
{
public:
    SettingsPanel();

    void Initialize() override;
    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd) override;
    void UpdateLayout(HWND hwnd) override;
    void OnMouseMove(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonUp(HWND hwnd) override;

private:
    D2D1_RECT_F rowRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F toggleRect_ = D2D1::RectF(0, 0, 0, 0);
    bool rowHovered_ = false;
};
