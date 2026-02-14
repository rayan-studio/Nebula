#include "Theme.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace UI::Theme
{
    namespace
    {
        bool g_initialized = false;
        Mode g_mode = Mode::Dark;
        bool g_windowFocused = true;
        Palette g_dark = {};
        Palette g_light = {};

        std::filesystem::path GetThemeStorePath()
        {
            PWSTR appDataPath = nullptr;
            std::filesystem::path out;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)) && appDataPath)
            {
                std::filesystem::path base(appDataPath);
                CoTaskMemFree(appDataPath);
                out = base / L"Nebula";
                std::error_code ec;
                std::filesystem::create_directories(out, ec);
                out /= L"theme.txt";
            }
            return out;
        }

        void SaveModeToDisk(Mode mode)
        {
            std::filesystem::path path = GetThemeStorePath();
            if (path.empty())
                return;

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
                return;

            const char *value = (mode == Mode::Light) ? "light" : "dark";
            out.write(value, (std::streamsize)strlen(value));
        }

        Mode LoadModeFromDisk()
        {
            std::filesystem::path path = GetThemeStorePath();
            if (path.empty() || !std::filesystem::exists(path))
                return Mode::Dark;

            std::ifstream in(path, std::ios::binary);
            if (!in)
                return Mode::Dark;

            std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (raw.empty())
                return Mode::Dark;

            std::string lowered = raw;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });

            if (lowered.find("light") != std::string::npos)
                return Mode::Light;
            return Mode::Dark;
        }

        void BuildDarkPalette(Palette &p)
        {
            p.titlebarBgFocused = D2D1::ColorF(0.06f, 0.07f, 0.09f, 1.0f);
            p.titlebarBgUnfocused = D2D1::ColorF(0.11f, 0.11f, 0.12f, 1.0f);
            p.titlebarBorderFocused = D2D1::ColorF(0.24f, 0.24f, 0.26f, 1.0f);
            p.titlebarBorderUnfocused = D2D1::ColorF(0.74f, 0.78f, 0.84f, 1.0f);
            p.titlebarTextFocused = D2D1::ColorF(0.90f, 0.93f, 0.98f, 1.0f);
            p.titlebarTextUnfocused = D2D1::ColorF(0.68f, 0.72f, 0.78f, 1.0f);
            p.titlebarIconFocused = D2D1::ColorF(0.95f, 0.97f, 1.0f, 1.0f);
            p.titlebarIconUnfocused = D2D1::ColorF(0.75f, 0.78f, 0.84f, 1.0f);
            p.titlebarCenterTitleFocused = D2D1::ColorF(0.95f, 0.97f, 1.0f, 1.0f);
            p.titlebarCenterTitleUnfocused = D2D1::ColorF(0.82f, 0.86f, 0.92f, 0.96f);

            // Non-titlebar UI stays stable regardless of focus.
            p.chromeBgFocused = D2D1::ColorF(18.0f / 255.0f, 18.0f / 255.0f, 18.0f / 255.0f, 1.0f);
            p.chromeBgUnfocused = p.chromeBgFocused;
            p.chromeBorderFocused = D2D1::ColorF(48.0f / 255.0f, 48.0f / 255.0f, 48.0f / 255.0f, 1.0f);
            p.chromeBorderUnfocused = p.chromeBorderFocused;
            p.textPrimaryFocused = D2D1::ColorF(204.0f / 255.0f, 204.0f / 255.0f, 204.0f / 255.0f, 1.0f);
            p.textPrimaryUnfocused = p.textPrimaryFocused;
            p.textMutedFocused = D2D1::ColorF(0.60f, 0.60f, 0.60f, 1.0f);
            p.textMutedUnfocused = p.textMutedFocused;

            p.accent = D2D1::ColorF(0.24f, 0.57f, 0.92f, 1.0f);
            p.accentStrong = D2D1::ColorF(0.00f, 0.80f, 1.0f, 1.0f);

            p.sidebarIconNormal = D2D1::ColorF(0x9e9e9e);
            p.sidebarIconHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            p.sidebarIconActive = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            p.sidebarHoverBg = D2D1::ColorF(0x2d2d2d);
            p.sidebarActiveBg = D2D1::ColorF(0x3c3f41);
            p.sidebarIndicator = D2D1::ColorF(0x4A9FEB);

            p.explorerRowHover = D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f);
            p.explorerRowActive = D2D1::ColorF(0.12f, 0.18f, 0.25f, 1.0f);
            p.explorerGuide = D2D1::ColorF(42.0f / 255.0f, 42.0f / 255.0f, 42.0f / 255.0f, 0.9f);
            p.explorerToolbarHover = D2D1::ColorF(0x2a2d2e);
            p.explorerPlaceholderText = D2D1::ColorF(0.60f, 0.60f, 0.60f, 1.0f);

            p.inputBackground = D2D1::ColorF(0.10f, 0.10f, 0.10f, 1.0f);
            p.inputBorder = D2D1::ColorF(0.26f, 0.26f, 0.26f, 1.0f);
            p.inputFocusBorder = p.accent;
            p.inputText = D2D1::ColorF(0.93f, 0.93f, 0.93f, 1.0f);
            p.inputPlaceholder = D2D1::ColorF(0.56f, 0.56f, 0.56f, 1.0f);
            p.inputSelection = D2D1::ColorF(0.20f, 0.57f, 1.0f, 0.27f);
            p.inputCaret = D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f);
        }

        void BuildLightPalette(Palette &p)
        {
            // Keep titlebar values unchanged for now (explicit user request).
            p.titlebarBgFocused = D2D1::ColorF(0.06f, 0.07f, 0.09f, 1.0f);
            p.titlebarBgUnfocused = D2D1::ColorF(0.11f, 0.11f, 0.12f, 1.0f);
            p.titlebarBorderFocused = D2D1::ColorF(0.74f, 0.78f, 0.84f, 1.0f);
            p.titlebarBorderUnfocused = D2D1::ColorF(0.24f, 0.24f, 0.26f, 1.0f);
            p.titlebarTextFocused = D2D1::ColorF(0.90f, 0.93f, 0.98f, 1.0f);
            p.titlebarTextUnfocused = D2D1::ColorF(0.68f, 0.72f, 0.78f, 1.0f);
            p.titlebarIconFocused = D2D1::ColorF(0.95f, 0.97f, 1.0f, 1.0f);
            p.titlebarIconUnfocused = D2D1::ColorF(0.75f, 0.78f, 0.84f, 1.0f);
            p.titlebarCenterTitleFocused = D2D1::ColorF(0.95f, 0.97f, 1.0f, 1.0f);
            p.titlebarCenterTitleUnfocused = D2D1::ColorF(0.82f, 0.86f, 0.92f, 0.96f);

            // Non-titlebar UI stays stable regardless of focus.
            p.chromeBgFocused = D2D1::ColorF(0.95f, 0.96f, 0.98f, 1.0f);
            p.chromeBgUnfocused = p.chromeBgFocused;
            p.chromeBorderFocused = D2D1::ColorF(0.74f, 0.78f, 0.84f, 1.0f);
            p.chromeBorderUnfocused = p.chromeBorderFocused;
            p.textPrimaryFocused = D2D1::ColorF(0.14f, 0.18f, 0.24f, 1.0f);
            p.textPrimaryUnfocused = p.textPrimaryFocused;
            p.textMutedFocused = D2D1::ColorF(0.42f, 0.48f, 0.56f, 1.0f);
            p.textMutedUnfocused = p.textMutedFocused;

            p.accent = D2D1::ColorF(0.24f, 0.57f, 0.92f, 1.0f);
            p.accentStrong = D2D1::ColorF(0.08f, 0.63f, 0.96f, 1.0f);

            p.sidebarIconNormal = D2D1::ColorF(0.42f, 0.46f, 0.52f, 1.0f);
            p.sidebarIconHover = D2D1::ColorF(0.10f, 0.14f, 0.20f, 1.0f);
            p.sidebarIconActive = D2D1::ColorF(0.05f, 0.10f, 0.16f, 1.0f);
            p.sidebarHoverBg = D2D1::ColorF(0.84f, 0.88f, 0.94f, 1.0f);
            p.sidebarActiveBg = D2D1::ColorF(0.78f, 0.86f, 0.97f, 1.0f);
            p.sidebarIndicator = p.accent;

            p.explorerRowHover = D2D1::ColorF(0.86f, 0.90f, 0.96f, 1.0f);
            p.explorerRowActive = D2D1::ColorF(0.80f, 0.87f, 0.98f, 1.0f);
            p.explorerGuide = D2D1::ColorF(0.74f, 0.80f, 0.90f, 0.9f);
            p.explorerToolbarHover = D2D1::ColorF(0.84f, 0.88f, 0.94f, 1.0f);
            p.explorerPlaceholderText = D2D1::ColorF(0.42f, 0.46f, 0.52f, 1.0f);

            p.inputBackground = D2D1::ColorF(0.97f, 0.98f, 0.99f, 1.0f);
            p.inputBorder = D2D1::ColorF(0.72f, 0.76f, 0.82f, 1.0f);
            p.inputFocusBorder = p.accent;
            p.inputText = D2D1::ColorF(0.13f, 0.17f, 0.22f, 1.0f);
            p.inputPlaceholder = D2D1::ColorF(0.48f, 0.54f, 0.62f, 1.0f);
            p.inputSelection = D2D1::ColorF(0.20f, 0.57f, 1.0f, 0.22f);
            p.inputCaret = D2D1::ColorF(0.15f, 0.19f, 0.25f, 1.0f);
        }

        void EnsureInitialized()
        {
            if (g_initialized)
                return;
            BuildDarkPalette(g_dark);
            BuildLightPalette(g_light);
            g_mode = LoadModeFromDisk();
            g_initialized = true;
        }
    }

    void Initialize()
    {
        EnsureInitialized();
    }

    Mode GetMode()
    {
        EnsureInitialized();
        return g_mode;
    }

    void SetMode(Mode mode)
    {
        EnsureInitialized();
        if (g_mode == mode)
            return;
        g_mode = mode;
        SaveModeToDisk(g_mode);
    }

    void ToggleMode()
    {
        EnsureInitialized();
        SetMode(g_mode == Mode::Dark ? Mode::Light : Mode::Dark);
    }

    void SetWindowFocused(bool focused)
    {
        EnsureInitialized();
        g_windowFocused = focused;
    }

    bool IsWindowFocused()
    {
        EnsureInitialized();
        return g_windowFocused;
    }

    const Palette &GetPalette()
    {
        EnsureInitialized();
        return (g_mode == Mode::Light) ? g_light : g_dark;
    }

    D2D1_COLOR_F TitlebarBackground(bool focused)
    {
        const Palette &p = GetPalette();
        return focused ? p.titlebarBgFocused : p.titlebarBgUnfocused;
    }

    D2D1_COLOR_F TitlebarBorder(bool focused)
    {
        const Palette &p = GetPalette();
        return focused ? p.titlebarBorderFocused : p.titlebarBorderUnfocused;
    }

    D2D1_COLOR_F TitlebarText(bool focused)
    {
        const Palette &p = GetPalette();
        return focused ? p.titlebarTextFocused : p.titlebarTextUnfocused;
    }

    D2D1_COLOR_F TitlebarIcon(bool focused)
    {
        const Palette &p = GetPalette();
        return focused ? p.titlebarIconFocused : p.titlebarIconUnfocused;
    }

    D2D1_COLOR_F TitlebarCenterTitle(bool focused)
    {
        const Palette &p = GetPalette();
        return focused ? p.titlebarCenterTitleFocused : p.titlebarCenterTitleUnfocused;
    }

    D2D1_COLOR_F ChromeBackground()
    {
        const Palette &p = GetPalette();
        return IsWindowFocused() ? p.chromeBgFocused : p.chromeBgUnfocused;
    }

    D2D1_COLOR_F ChromeBorder()
    {
        const Palette &p = GetPalette();
        return IsWindowFocused() ? p.chromeBorderFocused : p.chromeBorderUnfocused;
    }

    D2D1_COLOR_F PrimaryText()
    {
        const Palette &p = GetPalette();
        return IsWindowFocused() ? p.textPrimaryFocused : p.textPrimaryUnfocused;
    }

    D2D1_COLOR_F MutedText()
    {
        const Palette &p = GetPalette();
        return IsWindowFocused() ? p.textMutedFocused : p.textMutedUnfocused;
    }

    D2D1_COLOR_F Accent()
    {
        return GetPalette().accent;
    }

    D2D1_COLOR_F AccentStrong()
    {
        return GetPalette().accentStrong;
    }
}
