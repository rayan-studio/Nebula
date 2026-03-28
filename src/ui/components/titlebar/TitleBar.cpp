#include "TitleBar.h"
#include "helpers/window_helpers.h"
#include "helpers/path_helpers.h"
#include "ui/components/menu/DropdownMenu.h"
#include "utils/auth/GitHubAuth.h"
#include "core/window/Window.h"
#include "core/explorer/Explorer.h"
#include "ui/theme/Theme.h"
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cwctype>

// Disable min/max macros from Windows headers
#undef min
#undef max

// Cache global pour l'icône
static ID2D1Bitmap *g_iconBitmap = nullptr;
static ID2D1Bitmap *g_titlebarGitHubBitmap = nullptr;
static ID2D1RenderTarget *g_titlebarGitHubCtx = nullptr;
static bool g_titlebarGitHubConnected = false;
static bool g_titlebarGitHubInit = false;
static DWORD g_titlebarGitHubLastCheckTick = 0;

// État global des menus
static bool IsGitHubConnectedForTitleBar()
{
    DWORD now = GetTickCount();
    if (!g_titlebarGitHubInit || (now - g_titlebarGitHubLastCheckTick) >= 1200)
    {
        g_titlebarGitHubConnected = GitHubAuth::HasToken();
        g_titlebarGitHubInit = true;
        g_titlebarGitHubLastCheckTick = now;
    }
    return g_titlebarGitHubConnected;
}

static ID2D1Bitmap *GetTitleBarGitHubBadgeIcon(ID2D1RenderTarget *ctx, UINT dpi)
{
    if (!ctx)
        return nullptr;

    if (g_titlebarGitHubCtx != ctx)
    {
        if (g_titlebarGitHubBitmap)
        {
            g_titlebarGitHubBitmap->Release();
            g_titlebarGitHubBitmap = nullptr;
        }
        g_titlebarGitHubCtx = ctx;
    }

    if (!g_titlebarGitHubBitmap)
    {
        int px = win32_dpi_scale(14, dpi);
        g_titlebarGitHubBitmap = GetExplorerManager().LoadSvgIconPublic(
            ctx, "assets\\ressource\\icons\\git.svg", px, dpi);
    }
    return g_titlebarGitHubBitmap;
}

// Helper pour charger l'icône
static std::wstring ResolveUiAssetPath(const wchar_t *filename)
{
    if (!filename || !*filename)
        return L"";

    std::filesystem::path requested(filename);
    if (requested.is_absolute())
        return requested.wstring();

    std::wstring generic = requested.generic_wstring();
    std::wstring lower = generic;
    for (wchar_t &ch : lower)
        ch = (wchar_t)towlower(ch);

    if (lower.rfind(L"assets/", 0) == 0)
    {
        std::filesystem::path rel = std::filesystem::path(generic).lexically_relative(std::filesystem::path(L"assets"));
        return NebulaAssetPath(rel).wstring();
    }

    return (NebulaExeDir() / requested).wstring();
}

static ID2D1Bitmap *LoadIconBitmap(ID2D1RenderTarget *ctx, const wchar_t *filename)
{
    if (g_iconBitmap)
        return g_iconBitmap;

    IWICImagingFactory *wicFactory = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
    if (!wicFactory)
        return nullptr;

    std::wstring resolvedPath = ResolveUiAssetPath(filename);
    IWICBitmapDecoder *decoder = nullptr;
    wicFactory->CreateDecoderFromFilename(resolvedPath.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);

    if (decoder)
    {
        IWICBitmapFrameDecode *frame = nullptr;
        decoder->GetFrame(0, &frame);

        if (frame)
        {
            IWICFormatConverter *converter = nullptr;
            wicFactory->CreateFormatConverter(&converter);

            if (converter)
            {
                converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
                ctx->CreateBitmapFromWicBitmap(converter, NULL, &g_iconBitmap);
                converter->Release();
            }
            frame->Release();
        }
        decoder->Release();
    }

    wicFactory->Release();
    return g_iconBitmap;
}

// Fonction helper pour dessiner l'icône minimize (ligne horizontale)
static void DrawMinimizeIcon(ID2D1RenderTarget *ctx, ID2D1SolidColorBrush *brush, D2D1_RECT_F rect)
{
    float centerX = std::round((rect.left + rect.right) / 2.0f);
    float centerY = std::round((rect.top + rect.bottom) / 2.0f);
    float lineWidth = 10.0f;

    float left = std::floor(centerX - lineWidth / 2.0f) + 0.5f;
    float right = std::floor(centerX + lineWidth / 2.0f) + 0.5f;
    float y = std::floor(centerY) + 0.5f;
    D2D1_RECT_F lineRect = D2D1::RectF(left, y - 0.5f, right, y + 0.5f);

    ctx->FillRectangle(lineRect, brush);
}

// Fonction helper pour dessiner l'icône maximize (carré simple)
static void DrawMaximizeIcon(ID2D1RenderTarget *ctx, ID2D1SolidColorBrush *brush, D2D1_RECT_F rect)
{
    float centerX = std::round((rect.left + rect.right) / 2.0f);
    float centerY = std::round((rect.top + rect.bottom) / 2.0f);
    float size = 10.0f;

    float half = size / 2.0f;
    float left = std::floor(centerX - half) + 0.5f;
    float right = std::floor(centerX + half) + 0.5f;
    float top = std::floor(centerY - half) + 0.5f;
    float bottom = std::floor(centerY + half) + 0.5f;

    D2D1_RECT_F iconRect = D2D1::RectF(left, top, right, bottom);

    ctx->DrawRectangle(iconRect, brush, 1.0f);
}

// Fonction helper pour dessiner l'icône restore (style Windows classique)
static void DrawRestoreIcon(
    ID2D1RenderTarget *ctx,
    IDWriteFactory *dwrite,
    ID2D1SolidColorBrush *brush,
    D2D1_RECT_F rect)
{
    if (!dwrite || !ctx || !brush)
        return;

    float height = rect.bottom - rect.top;
    float fontSize = std::clamp(height * 0.3f, 10.0f, height - 2.0f);

    IDWriteTextFormat *iconFormat = nullptr;
    if (SUCCEEDED(dwrite->CreateTextFormat(
            L"Segoe MDL2 Assets", NULL,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, L"en-us", &iconFormat)))
    {
        iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const wchar_t glyph[2] = {0xE923, 0};

        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        ctx->DrawTextW(glyph, 1, iconFormat, rect, brush,
                       D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

        iconFormat->Release();
    }
}

// Fonction helper pour dessiner l'icône close (X)
static void DrawCloseIcon(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, ID2D1SolidColorBrush *brush, D2D1_RECT_F rect)
{
    if (!dwrite || !ctx || !brush)
        return;

    float height = rect.bottom - rect.top;
    float fontSize = std::clamp(height * 0.36f, 10.0f, height - 4.0f);

    IDWriteTextFormat *iconFormat = nullptr;
    if (SUCCEEDED(dwrite->CreateTextFormat(
            L"Segoe MDL2 Assets", NULL,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, L"en-us", &iconFormat)))
    {
        iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const wchar_t glyph[2] = {0xE8BB, 0}; // Close icon

        D2D1_TEXT_ANTIALIAS_MODE prevTextAA = ctx->GetTextAntialiasMode();
        ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
        ctx->DrawTextW(glyph, 1, iconFormat, rect, brush,
                       D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        ctx->SetTextAntialiasMode(prevTextAA);

        iconFormat->Release();
    }
}

// Fonction helper pour dessiner l'icône play (triangle)
static void DrawPlayIcon(
    ID2D1RenderTarget *ctx,
    IDWriteFactory *dwrite,
    ID2D1SolidColorBrush *brush,
    D2D1_RECT_F rect)
{
    if (!dwrite || !ctx || !brush)
        return;

    float height = rect.bottom - rect.top;
    float fontSize = std::clamp(height * 0.45f, 12.0f, height - 2.0f);

    IDWriteTextFormat *iconFormat = nullptr;
    if (SUCCEEDED(dwrite->CreateTextFormat(
            L"Segoe MDL2 Assets", NULL,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, L"en-us", &iconFormat)))
    {
        iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const wchar_t glyph[2] = {0xE768, 0}; // Play icon

        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        ctx->DrawTextW(glyph, 1, iconFormat, rect, brush,
                       D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

        iconFormat->Release();
    }
}

static void DrawDebugIcon(
    ID2D1RenderTarget *ctx,
    ID2D1SolidColorBrush *brush,
    D2D1_RECT_F rect)
{
    if (!ctx || !brush)
        return;

    const float cx = std::round((rect.left + rect.right) * 0.5f);
    const float cy = std::round((rect.top + rect.bottom) * 0.5f);
    const float r = 4.5f;

    ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, 1.2f);
    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 1.2f, 1.2f), brush);
}

void DrawCustomTitleBarD2D(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd, int hoveredButton, bool hasFocus, const std::wstring &title)
{
    (void)title;
    if (!ctx)
        return;

    RECT title_bar_rect = win32_titlebar_rect(hwnd);
    D2D1_RECT_F tb = D2D1::RectF((FLOAT)title_bar_rect.left, (FLOAT)title_bar_rect.top, (FLOAT)title_bar_rect.right, (FLOAT)title_bar_rect.bottom);

    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
    D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    Window *window = GetWindowFromHwnd(hwnd);
    float aMin = window ? window->GetTitlebarHoverAlpha(Window::Hovered_Minimize) : (hoveredButton == Window::Hovered_Minimize ? 1.0f : 0.0f);
    float aMax = window ? window->GetTitlebarHoverAlpha(Window::Hovered_Maximize) : (hoveredButton == Window::Hovered_Maximize ? 1.0f : 0.0f);
    float aClose = window ? window->GetTitlebarHoverAlpha(Window::Hovered_Close) : (hoveredButton == Window::Hovered_Close ? 1.0f : 0.0f);
    float aDebug = window ? window->GetTitlebarHoverAlpha(Window::Hovered_Debug) : (hoveredButton == Window::Hovered_Debug ? 1.0f : 0.0f);
    float aRun = window ? window->GetTitlebarHoverAlpha(Window::Hovered_Run) : (hoveredButton == Window::Hovered_Run ? 1.0f : 0.0f);
    const D2D1_COLOR_F titlebarBg = UI::Theme::TitlebarBackground(hasFocus);
    const D2D1_COLOR_F titlebarText = UI::Theme::TitlebarText(hasFocus);
    const D2D1_COLOR_F titlebarIcon = UI::Theme::TitlebarIcon(hasFocus);
    const UI::Theme::Palette &themePalette = UI::Theme::GetPalette();
    const float titlebarHoverOpacity = (UI::Theme::GetMode() == UI::Theme::Mode::Light) ? 0.88f : 0.75f;
    if (window && window->IsNewProjectOverlayVisible())
    {
        // Minimal titlebar for the new-project screen (no editor menus).
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        ID2D1SolidColorBrush *bgBrush = nullptr;
        ctx->CreateSolidColorBrush(titlebarBg, &bgBrush);

        if (bgBrush)
            ctx->FillRectangle(tb, bgBrush);

        CustomTitleBarButtonRects button_rects = win32_get_title_bar_button_rects(hwnd, &title_bar_rect);
        D2D1_RECT_F rMin = D2D1::RectF((FLOAT)button_rects.minimize.left, (FLOAT)button_rects.minimize.top, (FLOAT)button_rects.minimize.right, (FLOAT)button_rects.minimize.bottom);
        D2D1_RECT_F rMax = D2D1::RectF((FLOAT)button_rects.maximize.left, (FLOAT)button_rects.maximize.top, (FLOAT)button_rects.maximize.right, (FLOAT)button_rects.maximize.bottom);
        D2D1_RECT_F rClose = D2D1::RectF((FLOAT)button_rects.close.left, (FLOAT)button_rects.close.top, (FLOAT)button_rects.close.right, (FLOAT)button_rects.close.bottom);

        ID2D1SolidColorBrush *hoverBrush = nullptr;
        ID2D1SolidColorBrush *closeHoverBrush = nullptr;
        D2D1_COLOR_F hoverColor = themePalette.explorerToolbarHover;
        hoverColor.a = titlebarHoverOpacity * aMin;
        ctx->CreateSolidColorBrush(hoverColor, &hoverBrush);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0xe81123, 0.85f * aClose), &closeHoverBrush);

        if (hoverBrush && aMin > 0.01f)
            ctx->FillRectangle(rMin, hoverBrush);
        if (hoverBrush && aMax > 0.01f)
        {
            D2D1_COLOR_F maxHoverColor = themePalette.explorerToolbarHover;
            maxHoverColor.a = titlebarHoverOpacity * aMax;
            hoverBrush->SetColor(maxHoverColor);
            ctx->FillRectangle(rMax, hoverBrush);
        }
        if (closeHoverBrush && aClose > 0.01f)
            ctx->FillRectangle(rClose, closeHoverBrush);

        ID2D1SolidColorBrush *iconBrush = nullptr;
        ctx->CreateSolidColorBrush(titlebarIcon, &iconBrush);
        if (iconBrush)
        {
            ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            DrawMinimizeIcon(ctx, iconBrush, rMin);
            if (win32_window_is_maximized(hwnd))
                DrawRestoreIcon(ctx, dwrite, iconBrush, rMax);
            else
                DrawMaximizeIcon(ctx, iconBrush, rMax);
            DrawCloseIcon(ctx, dwrite, iconBrush, rClose);
        }

        // App icon + title
        ID2D1Bitmap *iconBitmap = LoadIconBitmap(ctx, L"assets/favicon.ico");
        UINT dpi = win32_get_dpi_for_window(hwnd);
        float padding = (float)win32_dpi_scale(12, dpi);
        float currentX = tb.left + padding;
        if (iconBitmap)
        {
            float iconSize = 16.0f;
            D2D1_RECT_F iconRect = D2D1::RectF(
                std::round(currentX),
                std::round((tb.top + tb.bottom - iconSize) / 2.0f),
                std::round(currentX + iconSize),
                std::round((tb.top + tb.bottom + iconSize) / 2.0f));
            ctx->DrawBitmap(iconBitmap, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            currentX += iconSize + 8.0f;
        }

        if (dwrite)
        {
            IDWriteTextFormat *titleFmt = nullptr;
            dwrite->CreateTextFormat(
                L"Segoe UI Variable Text",
                NULL,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                12.5f,
                L"en-us",
                &titleFmt);
            if (titleFmt)
            {
                titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F titleRect = D2D1::RectF(currentX, tb.top, rMax.left - 10.0f, tb.bottom);
                ID2D1SolidColorBrush *titleBrush = nullptr;
                ctx->CreateSolidColorBrush(titlebarText, &titleBrush);
                if (titleBrush)
                {
                    ctx->DrawTextW(L"Nebula - Commencez", 19, titleFmt, titleRect, titleBrush);
                    titleBrush->Release();
                }
                titleFmt->Release();
            }
        }

        if (iconBrush) iconBrush->Release();
        if (hoverBrush) hoverBrush->Release();
        if (closeHoverBrush) closeHoverBrush->Release();
        if (bgBrush) bgBrush->Release();

        ctx->SetAntialiasMode(oldAA);
        ctx->SetTextAntialiasMode(oldTextAA);
        return;
    }

    // Background - dark theme
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(titlebarBg, &bgBrush);
    ctx->FillRectangle(tb, bgBrush);

    CustomTitleBarButtonRects button_rects = win32_get_title_bar_button_rects(hwnd, &title_bar_rect);

    D2D1_RECT_F rMin = D2D1::RectF((FLOAT)button_rects.minimize.left, (FLOAT)button_rects.minimize.top, (FLOAT)button_rects.minimize.right, (FLOAT)button_rects.minimize.bottom);
    D2D1_RECT_F rMax = D2D1::RectF((FLOAT)button_rects.maximize.left, (FLOAT)button_rects.maximize.top, (FLOAT)button_rects.maximize.right, (FLOAT)button_rects.maximize.bottom);
    D2D1_RECT_F rClose = D2D1::RectF((FLOAT)button_rects.close.left, (FLOAT)button_rects.close.top, (FLOAT)button_rects.close.right, (FLOAT)button_rects.close.bottom);
    D2D1_RECT_F rDebug = D2D1::RectF((FLOAT)button_rects.debug.left, (FLOAT)button_rects.debug.top, (FLOAT)button_rects.debug.right, (FLOAT)button_rects.debug.bottom);
    D2D1_RECT_F rRun = D2D1::RectF((FLOAT)button_rects.run.left, (FLOAT)button_rects.run.top, (FLOAT)button_rects.run.right, (FLOAT)button_rects.run.bottom);

    ID2D1SolidColorBrush *hoverBrush = nullptr;
    D2D1_COLOR_F hoverColor = themePalette.explorerToolbarHover;
    hoverColor.a = titlebarHoverOpacity * aMin;
    ctx->CreateSolidColorBrush(hoverColor, &hoverBrush);

    ID2D1SolidColorBrush *closeHoverBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0xe81123, 0.85f * aClose), &closeHoverBrush);

    if (hoverBrush && aMin > 0.01f)
        ctx->FillRectangle(rMin, hoverBrush);
    if (hoverBrush && aDebug > 0.01f)
    {
        D2D1_COLOR_F debugHoverColor = themePalette.explorerToolbarHover;
        debugHoverColor.a = titlebarHoverOpacity * aDebug;
        hoverBrush->SetColor(debugHoverColor);
        ctx->FillRectangle(rDebug, hoverBrush);
    }
    if (hoverBrush && aRun > 0.01f)
    {
        D2D1_COLOR_F runHoverColor = themePalette.explorerToolbarHover;
        runHoverColor.a = titlebarHoverOpacity * aRun;
        hoverBrush->SetColor(runHoverColor);
        ctx->FillRectangle(rRun, hoverBrush);
    }
    if (hoverBrush && aMax > 0.01f)
    {
        D2D1_COLOR_F maxHoverColor = themePalette.explorerToolbarHover;
        maxHoverColor.a = titlebarHoverOpacity * aMax;
        hoverBrush->SetColor(maxHoverColor);
        ctx->FillRectangle(rMax, hoverBrush);
    }
    if (closeHoverBrush && aClose > 0.01f)
        ctx->FillRectangle(rClose, closeHoverBrush);

    UINT dpi = win32_get_dpi_for_window(hwnd);
    bool isMaximized = win32_window_is_maximized(hwnd);

    // Brush pour les icônes
    ID2D1SolidColorBrush *iconBrush = nullptr;
    ctx->CreateSolidColorBrush(titlebarIcon, &iconBrush);

    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    // Dessiner les icônes
    if (iconBrush)
    {
        DrawMinimizeIcon(ctx, iconBrush, rMin);
        DrawDebugIcon(ctx, iconBrush, rDebug);
        DrawPlayIcon(ctx, dwrite, iconBrush, rRun);

        if (isMaximized)
            DrawRestoreIcon(ctx, dwrite, iconBrush, rMax);
        else
            DrawMaximizeIcon(ctx, iconBrush, rMax);

        DrawCloseIcon(ctx, dwrite, iconBrush, rClose);
    }

    // Icône de l'app
    ID2D1Bitmap *iconBitmap = LoadIconBitmap(ctx, L"assets/favicon.ico");
    float padding = (float)win32_dpi_scale(12, dpi);
    if (isMaximized)
    {
        padding += (float)win32_dpi_scale(6, dpi);
    }

    float currentX = tb.left + padding;

    if (iconBitmap)
    {
        float iconSize = 16.0f;
        D2D1_RECT_F iconRect = D2D1::RectF(
            std::round(currentX),
            std::round((tb.top + tb.bottom - iconSize) / 2.0f),
            std::round(currentX + iconSize),
            std::round((tb.top + tb.bottom + iconSize) / 2.0f));
        ctx->DrawBitmap(iconBitmap, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        currentX += iconSize + 8.0f;
    }

    // ============================================================================
    // JETBRAINS MONO - Format pour les menus
    // Utilisation de Regular (400) avec taille optimisée pour la lisibilité
    // ============================================================================
    IDWriteTextFormat *menuFormat = nullptr;
    if (dwrite)
    {
        dwrite->CreateTextFormat(
            L"JetBrains Mono",
            NULL,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.5f,
            L"en-us",
            &menuFormat);

        if (menuFormat)
        {
            menuFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            menuFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    // Menu items (Terminal removed)
    std::vector<std::wstring> menus = {L"File", L"Edit", L"Tampon", L"Selection"};

    // Keep cached menu list in sync with the current top-level menu model.
    auto &menuItems = GetMenuItems();
    if (menuItems.size() != menus.size())
    {
        menuItems.clear();
        for (const auto &menu : menus)
        {
            menuItems.push_back({menu, D2D1::RectF(0, 0, 0, 0), false});
        }
    }

    ID2D1SolidColorBrush *menuTextBrush = nullptr;
    ID2D1SolidColorBrush *menuHoverBrush = nullptr;
    ctx->CreateSolidColorBrush(titlebarText, &menuTextBrush);
    ctx->CreateSolidColorBrush(themePalette.explorerToolbarHover, &menuHoverBrush);

    // Dessiner chaque menu item
    for (size_t i = 0; i < menuItems.size(); i++)
    {
        auto &item = menuItems[i];

        if (dwrite && menuFormat)
        {
            // Calculer la largeur du texte
            IDWriteTextLayout *textLayout = nullptr;
            dwrite->CreateTextLayout(item.label.c_str(), (UINT32)item.label.size(), menuFormat, 1000.0f, 30.0f, &textLayout);

            float itemWidth = 16.0f;

            if (textLayout)
            {
                DWRITE_TEXT_METRICS metrics;
                textLayout->GetMetrics(&metrics);
                // JetBrains Mono est monospace, donc padding uniforme
                itemWidth = metrics.width + 24.0f;
                textLayout->Release();
            }

            item.rect = D2D1::RectF(
                std::round(currentX),
                tb.top,
                std::round(currentX + itemWidth),
                tb.bottom);

            if (item.hovered)
            {
                if (menuHoverBrush)
                {
                    D2D1_RECT_F hoverRect = D2D1::RectF(
                        item.rect.left + 4.0f,
                        item.rect.top + 6.0f,
                        item.rect.right - 4.0f,
                        item.rect.bottom - 6.0f);
                    D2D1_ROUNDED_RECT hoverRounded = D2D1::RoundedRect(hoverRect, 4.0f, 4.0f);
                    ctx->FillRoundedRectangle(hoverRounded, menuHoverBrush);
                }
            }

            // Dessiner le texte
            D2D1_RECT_F textRect = D2D1::RectF(
                item.rect.left + 10.0f,
                item.rect.top,
                item.rect.right - 10.0f,
                item.rect.bottom);

            ID2D1SolidColorBrush *textBrushToUse = menuTextBrush;
            if (item.hovered)
            {
                ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrushToUse);
            }

            ctx->DrawTextW(
                item.label.c_str(),
                (UINT32)item.label.size(),
                menuFormat,
                textRect,
                textBrushToUse,
                D2D1_DRAW_TEXT_OPTIONS_NONE,
                DWRITE_MEASURING_MODE_NATURAL);

            if (item.hovered && textBrushToUse != menuTextBrush)
                textBrushToUse->Release();

            currentX += itemWidth;
        }
    }

    const bool githubConnected = IsGitHubConnectedForTitleBar();
    D2D1_RECT_F githubBadgeRect = D2D1::RectF(0, 0, 0, 0);
    if (githubConnected)
    {
        float badgeHeight = (float)win32_dpi_scale(22, dpi);
        float badgeWidth = (float)win32_dpi_scale(114, dpi);
        float badgeRight = rDebug.left - (float)win32_dpi_scale(10, dpi);
        float badgeTop = std::round((tb.top + tb.bottom - badgeHeight) * 0.5f);
        githubBadgeRect = D2D1::RectF(
            std::round(badgeRight - badgeWidth),
            badgeTop,
            std::round(badgeRight),
            std::round(badgeTop + badgeHeight));
    }

    float titleLeft = currentX;
    float titleRight = githubConnected ? (githubBadgeRect.left - (float)win32_dpi_scale(10, dpi)) : rDebug.left;
    float titleWidth = titleRight - titleLeft;
    // Reuse menuFormat to avoid double rendering and keep ClearType
    if (menuFormat && titleWidth > 40.0f)
    {
        D2D1_RECT_F titleRect = D2D1::RectF(
            std::round(titleLeft),
            std::round(tb.top),
            std::round(titleRight),
            std::round(tb.bottom));

        std::wstring displayTitle;
        std::wstring rootPath = GetExplorerManager().GetState().rootPath;
        if (!rootPath.empty())
        {
            size_t pos = rootPath.find_last_of(L"\\/");
            std::wstring projectName = (pos != std::wstring::npos) ? rootPath.substr(pos + 1) : rootPath;
            displayTitle = title + L" - " + projectName;
        }
        else
        {
            displayTitle = title + L" - Aucun projet ouvert";
        }

        ID2D1SolidColorBrush *titleBrush = nullptr;
        D2D1_COLOR_F titleColor = UI::Theme::TitlebarCenterTitle(hasFocus);
        ctx->CreateSolidColorBrush(titleColor, &titleBrush);

        // Créer un format temporaire avec CENTER alignment
        IDWriteTextFormat *centerFormat = nullptr;
        dwrite->CreateTextFormat(
            L"Segoe UI", NULL,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"en-us", &centerFormat);

        if (centerFormat)
        {
            centerFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            centerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            D2D1_TEXT_ANTIALIAS_MODE prevTextAA = ctx->GetTextAntialiasMode();
            // Grayscale avoids color fringing on custom titlebars and often looks cleaner.
            ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            ctx->DrawTextW(displayTitle.c_str(), (UINT32)displayTitle.size(),
                           centerFormat, titleRect, titleBrush,
                           D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
            ctx->SetTextAntialiasMode(prevTextAA);

            centerFormat->Release();
        }

        if (titleBrush)
            titleBrush->Release();
    }

    if (githubConnected)
        window ? window->SetGitHubBadgeRect(githubBadgeRect) : void();
    else if (window)
        window->ClearGitHubBadgeRect();

    if (githubConnected && menuFormat)
    {
        const bool badgeHovered = window && window->IsGitHubBadgeHovered();
        float badgeAlpha = hasFocus ? 1.0f : 0.82f;
        ID2D1SolidColorBrush *badgeBg = nullptr;
        ID2D1SolidColorBrush *badgeBorder = nullptr;
        ID2D1SolidColorBrush *badgeText = nullptr;
        ID2D1SolidColorBrush *badgeDot = nullptr;
        D2D1_COLOR_F badgeBgColor = badgeHovered ? themePalette.explorerToolbarHover : themePalette.inputBackground;
        badgeBgColor.a = 0.95f * badgeAlpha;
        D2D1_COLOR_F badgeBorderColor = badgeHovered ? UI::Theme::Accent() : UI::Theme::ChromeBorder();
        badgeBorderColor.a = (badgeHovered ? 1.0f : 0.90f) * badgeAlpha;
        D2D1_COLOR_F badgeTextColor = UI::Theme::PrimaryText();
        badgeTextColor.a = badgeAlpha;
        D2D1_COLOR_F badgeDotColor = UI::Theme::AccentStrong();
        badgeDotColor.a = badgeAlpha;
        ctx->CreateSolidColorBrush(badgeBgColor, &badgeBg);
        ctx->CreateSolidColorBrush(badgeBorderColor, &badgeBorder);
        ctx->CreateSolidColorBrush(badgeTextColor, &badgeText);
        ctx->CreateSolidColorBrush(badgeDotColor, &badgeDot);

        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(githubBadgeRect, 6.0f, 6.0f);
        if (badgeBg)
            ctx->FillRoundedRectangle(rr, badgeBg);
        if (badgeBorder)
            ctx->DrawRoundedRectangle(rr, badgeBorder, 1.0f);

        ID2D1Bitmap *ghBmp = GetTitleBarGitHubBadgeIcon(ctx, dpi);
        if (ghBmp)
        {
            float iconSize = (float)win32_dpi_scale(14, dpi);
            float iconLeft = std::round(githubBadgeRect.left + (float)win32_dpi_scale(8, dpi));
            float iconTop = std::round((githubBadgeRect.top + githubBadgeRect.bottom - iconSize) * 0.5f);
            D2D1_RECT_F iconRect = D2D1::RectF(iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize);
            ctx->DrawBitmap(ghBmp, iconRect, badgeAlpha, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }

        if (badgeText)
        {
            D2D1_RECT_F textRect = D2D1::RectF(
                std::round(githubBadgeRect.left + (float)win32_dpi_scale(28, dpi)),
                githubBadgeRect.top,
                std::round(githubBadgeRect.right - (float)win32_dpi_scale(14, dpi)),
                githubBadgeRect.bottom);
            ctx->DrawTextW(L"GitHub", 6, menuFormat, textRect, badgeText,
                           D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }

        if (badgeDot)
        {
            float cx = std::round(githubBadgeRect.right - (float)win32_dpi_scale(8, dpi));
            float cy = std::round((githubBadgeRect.top + githubBadgeRect.bottom) * 0.5f);
            float r = (float)win32_dpi_scale(2, dpi);
            ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), badgeDot);
        }

        if (badgeDot)
            badgeDot->Release();
        if (badgeText)
            badgeText->Release();
        if (badgeBorder)
            badgeBorder->Release();
        if (badgeBg)
            badgeBg->Release();
    }
    if (menuHoverBrush)
        menuHoverBrush->Release();
    if (menuTextBrush)
        menuTextBrush->Release();
    if (menuFormat)
        menuFormat->Release();
    if (iconBrush)
        iconBrush->Release();
    ctx->SetAntialiasMode(oldAA);
    ctx->SetTextAntialiasMode(oldTextAA);
    if (closeHoverBrush)
        closeHoverBrush->Release();
    if (hoverBrush)
        hoverBrush->Release();
    if (bgBrush)
        bgBrush->Release();
}
