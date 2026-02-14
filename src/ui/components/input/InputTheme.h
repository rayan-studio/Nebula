#pragma once

#include <d2d1.h>
#include "ui/theme/Theme.h"

namespace UI::InputTheme
{
    inline constexpr float kCornerRadius = 6.0f;
    inline constexpr float kFontSize = 13.0f;
    inline constexpr float kHorizontalPadding = 10.0f;
    inline constexpr const wchar_t *kFontFamily = L"Segoe UI";

    inline D2D1_COLOR_F Background()
    {
        return Theme::GetPalette().inputBackground;
    }

    inline D2D1_COLOR_F Border()
    {
        return Theme::GetPalette().inputBorder;
    }

    inline D2D1_COLOR_F FocusBorder()
    {
        return Theme::GetPalette().inputFocusBorder;
    }

    inline D2D1_COLOR_F Text()
    {
        return Theme::GetPalette().inputText;
    }

    inline D2D1_COLOR_F Placeholder()
    {
        return Theme::GetPalette().inputPlaceholder;
    }

    inline D2D1_COLOR_F Selection()
    {
        return Theme::GetPalette().inputSelection;
    }

    inline D2D1_COLOR_F Caret()
    {
        return Theme::GetPalette().inputCaret;
    }
}
