#pragma once

#include <d2d1.h>

namespace UI::InputTheme
{
    inline constexpr float kCornerRadius = 6.0f;
    inline constexpr float kFontSize = 13.0f;
    inline constexpr float kHorizontalPadding = 10.0f;
    inline constexpr const wchar_t *kFontFamily = L"Segoe UI";

    inline D2D1_COLOR_F Background()
    {
        return D2D1::ColorF(0.10f, 0.10f, 0.10f, 1.0f);
    }

    inline D2D1_COLOR_F Border()
    {
        return D2D1::ColorF(0.26f, 0.26f, 0.26f, 1.0f);
    }

    inline D2D1_COLOR_F FocusBorder()
    {
        return D2D1::ColorF(0.24f, 0.57f, 0.92f, 1.0f);
    }

    inline D2D1_COLOR_F Text()
    {
        return D2D1::ColorF(0.93f, 0.93f, 0.93f, 1.0f);
    }

    inline D2D1_COLOR_F Placeholder()
    {
        return D2D1::ColorF(0.56f, 0.56f, 0.56f, 1.0f);
    }

    inline D2D1_COLOR_F Selection()
    {
        return D2D1::ColorF(0.20f, 0.57f, 1.0f, 0.27f);
    }

    inline D2D1_COLOR_F Caret()
    {
        return D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f);
    }
}
