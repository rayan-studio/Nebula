#pragma once

#include <d2d1.h>

namespace UI::Theme
{
    enum class Mode
    {
        Dark = 0,
        Light = 1,
    };

    struct Palette
    {
        D2D1_COLOR_F titlebarBgFocused;
        D2D1_COLOR_F titlebarBgUnfocused;
        D2D1_COLOR_F titlebarBorderFocused;
        D2D1_COLOR_F titlebarBorderUnfocused;
        D2D1_COLOR_F titlebarTextFocused;
        D2D1_COLOR_F titlebarTextUnfocused;
        D2D1_COLOR_F titlebarIconFocused;
        D2D1_COLOR_F titlebarIconUnfocused;
        D2D1_COLOR_F titlebarCenterTitleFocused;
        D2D1_COLOR_F titlebarCenterTitleUnfocused;

        D2D1_COLOR_F chromeBgFocused;
        D2D1_COLOR_F chromeBgUnfocused;
        D2D1_COLOR_F chromeBorderFocused;
        D2D1_COLOR_F chromeBorderUnfocused;
        D2D1_COLOR_F textPrimaryFocused;
        D2D1_COLOR_F textPrimaryUnfocused;
        D2D1_COLOR_F textMutedFocused;
        D2D1_COLOR_F textMutedUnfocused;

        D2D1_COLOR_F accent;
        D2D1_COLOR_F accentStrong;

        D2D1_COLOR_F sidebarIconNormal;
        D2D1_COLOR_F sidebarIconHover;
        D2D1_COLOR_F sidebarIconActive;
        D2D1_COLOR_F sidebarHoverBg;
        D2D1_COLOR_F sidebarActiveBg;
        D2D1_COLOR_F sidebarIndicator;

        D2D1_COLOR_F explorerRowHover;
        D2D1_COLOR_F explorerRowActive;
        D2D1_COLOR_F explorerGuide;
        D2D1_COLOR_F explorerToolbarHover;
        D2D1_COLOR_F explorerPlaceholderText;

        D2D1_COLOR_F inputBackground;
        D2D1_COLOR_F inputBorder;
        D2D1_COLOR_F inputFocusBorder;
        D2D1_COLOR_F inputText;
        D2D1_COLOR_F inputPlaceholder;
        D2D1_COLOR_F inputSelection;
        D2D1_COLOR_F inputCaret;
    };

    void Initialize();

    Mode GetMode();
    void SetMode(Mode mode);
    void ToggleMode();

    void SetWindowFocused(bool focused);
    bool IsWindowFocused();

    const Palette &GetPalette();

    D2D1_COLOR_F TitlebarBackground(bool focused);
    D2D1_COLOR_F TitlebarBorder(bool focused);
    D2D1_COLOR_F TitlebarText(bool focused);
    D2D1_COLOR_F TitlebarIcon(bool focused);
    D2D1_COLOR_F TitlebarCenterTitle(bool focused);

    D2D1_COLOR_F ChromeBackground();
    D2D1_COLOR_F ChromeBorder();
    D2D1_COLOR_F PrimaryText();
    D2D1_COLOR_F MutedText();

    D2D1_COLOR_F Accent();
    D2D1_COLOR_F AccentStrong();
}
